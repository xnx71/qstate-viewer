// C/C++ preprocessor.
//
// Structure
//   * Files are lexed once (cached) into Token vectors. Directives are recognized on the fly while tokens are pulled
//     from the file stack (rawNext); conditional compilation skips tokens there.
//   * Macro expansion follows the classic context-stack design: every expansion pushes a token context, a macro is
//     "disabled" while its context is on the stack, tokens naming a disabled macro are painted (never expanded again),
//     function-like macros look for their '(' beyond the end of exhausted contexts, arguments are pre-expanded in an
//     isolated context terminated by an end marker.
//   * Internally tokens are PTok: a pointer to an immutable Token (file cache, macro body or arena) plus file / line
//     overrides and flags, so nothing is copied until the final output vector is produced.
#include "qstate/cpp/preprocessor.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

namespace qstate::cpp {
namespace {

constexpr std::uint8_t kNoExpand = 1;
constexpr std::uint8_t kSpace = 2;
constexpr std::uint8_t kLineStart = 4;

// Total work budgets that stop runaway (exponential) macro expansion.
constexpr std::size_t kMaxExpansions = 100'000'000;

const Token kEndToken{TokKind::End, {}, 0, 0, true, false};
const Token kPlacemarker{TokKind::Punct, {}, 0, 0, false, false};

struct StrHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};
template <class V>
using StrMap = std::unordered_map<std::string, V, StrHash, std::equal_to<>>;

struct PTok {
    const Token* t = &kEndToken;
    std::uint32_t file = 0;
    std::uint32_t line = 0;
    std::uint8_t flags = 0;

    bool isEnd() const { return t->kind == TokKind::End; }
    bool isPlacemarker() const { return t == &kPlacemarker; }
    bool isIdent() const { return t->kind == TokKind::Ident; }
    bool isPunct(std::string_view p) const { return t->kind == TokKind::Punct && t->text == p; }
    bool isIdent(std::string_view s) const { return t->kind == TokKind::Ident && t->text == s; }
    bool space() const { return (flags & kSpace) != 0; }
    const std::string& text() const { return t->text; }
};

struct BodyItem {
    enum class Kind : std::uint8_t { Tok, Param, Stringize, Paste, VaOpen, VaClose };
    Kind kind = Kind::Tok;
    bool space = false;
    // Param / Stringize: parameter index. VaOpen: index of the matching VaClose item.
    std::uint32_t index = 0;
    Token tok; // Tok items only
};

enum class Builtin : std::uint8_t { None, File, Line, Counter, IncludeLevel, Pragma, HasInclude, HasAttribute };

struct Macro {
    std::string name;
    bool funcLike = false;
    bool variadic = false;
    bool disabled = false;
    Builtin builtin = Builtin::None;
    std::vector<std::string> params;
    std::vector<BodyItem> items;
};

bool sameDefinition(const Macro& a, const Macro& b) {
    if (a.builtin != b.builtin || a.funcLike != b.funcLike || a.variadic != b.variadic || a.params != b.params ||
        a.items.size() != b.items.size())
        return false;
    for (std::size_t i = 0; i < a.items.size(); ++i) {
        const BodyItem& x = a.items[i];
        const BodyItem& y = b.items[i];
        if (x.kind != y.kind || x.index != y.index) return false;
        if (i != 0 && x.space != y.space) return false;
        if (x.kind == BodyItem::Kind::Tok && (x.tok.kind != y.tok.kind || x.tok.text != y.tok.text)) return false;
    }
    return true;
}

std::string renderMacro(const Macro& m) {
    std::string out = m.name;
    if (m.funcLike) {
        out += '(';
        for (std::size_t i = 0; i < m.params.size(); ++i) {
            if (i) out += ',';
            if (m.variadic && i + 1 == m.params.size()) {
                if (m.params[i] != "__VA_ARGS__") out += m.params[i];
                out += "...";
            } else {
                out += m.params[i];
            }
        }
        out += ')';
    }
    std::string body;
    for (std::size_t i = 0; i < m.items.size(); ++i) {
        const BodyItem& it = m.items[i];
        if (!body.empty() && it.space) body += ' ';
        switch (it.kind) {
        case BodyItem::Kind::Tok: body += it.tok.text; break;
        case BodyItem::Kind::Param: body += m.params[it.index]; break;
        case BodyItem::Kind::Stringize: body += '#' + m.params[it.index]; break;
        case BodyItem::Kind::Paste: body += "##"; break;
        case BodyItem::Kind::VaOpen: body += "__VA_OPT__("; break;
        case BodyItem::Kind::VaClose: body += ')'; break;
        }
    }
    if (!body.empty()) {
        out += ' ';
        out += body;
    }
    return out;
}

// One lexed source file.
struct FileData {
    std::string path;
    std::uint32_t index = 0;
    std::vector<Token> tokens; // ends with an End token
    std::string guard;         // include-guard macro when the whole file is wrapped in `#ifndef GUARD ... #endif`
};

struct FileFrame {
    FileData* fd = nullptr;
    std::size_t pos = 0;
    std::size_t condBase = 0;
};

struct Cond {
    bool parentActive = true;
    bool active = false;
    bool taken = false;
    bool seenElse = false;
    std::uint32_t file = 0;
    std::uint32_t line = 0;
};

struct Context {
    std::vector<PTok> toks;
    std::size_t pos = 0;
    Macro* macro = nullptr;
};

struct Arg {
    std::vector<PTok> raw;
    std::vector<PTok> expanded;
    bool done = false;
    bool omitted = false; // variadic argument that was not passed at all (`F(a)` for `F(a, ...)`)
};

// ---------------------------------------------------------------------------------------------------------------------
// #if expression evaluation

struct Val {
    std::int64_t v = 0;
    bool u = false;
};

class ExprParser {
public:
    explicit ExprParser(const std::vector<PTok>& toks) : toks_(toks) {}

    // Returns false (with error()) on a malformed expression.
    bool parse(Val& out) {
        out = comma();
        if (err_.empty() && pos_ < toks_.size()) err_ = "missing binary operator before token '" + toks_[pos_].text() + "'";
        return err_.empty();
    }
    const std::string& error() const { return err_; }

private:
    const PTok* peek() const { return pos_ < toks_.size() ? &toks_[pos_] : nullptr; }
    bool peekPunct(std::string_view p) const {
        const PTok* t = peek();
        return t && t->isPunct(p);
    }
    void fail(const std::string& msg) {
        if (err_.empty()) err_ = msg;
    }

    Val comma() {
        Val v = ternary();
        while (err_.empty() && peekPunct(",")) {
            ++pos_;
            v = ternary();
        }
        return v;
    }

    // Recursion guard: pathological input such as thousands of '(' or '!' must not overflow the stack.
    struct DepthGuard {
        explicit DepthGuard(ExprParser& p) : parser(p) { ++parser.depth_; }
        ~DepthGuard() { --parser.depth_; }
        ExprParser& parser;
    };
    bool tooDeep() {
        if (depth_ <= 600) return false;
        fail("expression nested too deeply");
        return true;
    }

    Val ternary() {
        DepthGuard guard(*this);
        if (tooDeep()) return {};
        Val c = binary(1);
        if (!err_.empty() || !peekPunct("?")) return c;
        ++pos_;
        const bool takeFirst = c.v != 0;
        if (!takeFirst) ++skip_;
        Val a = comma();
        if (!takeFirst) --skip_;
        if (!err_.empty()) return a;
        if (!peekPunct(":")) {
            fail("'?' without following ':'");
            return a;
        }
        ++pos_;
        if (takeFirst) ++skip_;
        Val b = ternary();
        if (takeFirst) --skip_;
        Val r = takeFirst ? a : b;
        r.u = a.u || b.u;
        return r;
    }

    static int precedence(const PTok& t) {
        if (t.t->kind != TokKind::Punct) return -1;
        const std::string& s = t.text();
        if (s == "||") return 1;
        if (s == "&&") return 2;
        if (s == "|") return 3;
        if (s == "^") return 4;
        if (s == "&") return 5;
        if (s == "==" || s == "!=") return 6;
        if (s == "<" || s == ">" || s == "<=" || s == ">=") return 7;
        if (s == "<<" || s == ">>") return 8;
        if (s == "+" || s == "-") return 9;
        if (s == "*" || s == "/" || s == "%") return 10;
        return -1;
    }

    Val binary(int minPrec) {
        Val lhs = unary();
        while (err_.empty()) {
            const PTok* t = peek();
            if (!t) break;
            const int prec = precedence(*t);
            if (prec < minPrec) break;
            const std::string op = t->text();
            ++pos_;
            if (op == "||" || op == "&&") {
                const bool shortCircuit = op == "||" ? lhs.v != 0 : lhs.v == 0;
                if (shortCircuit) ++skip_;
                Val rhs = binary(prec + 1);
                if (shortCircuit) --skip_;
                const bool r = op == "||" ? (lhs.v != 0 || rhs.v != 0) : (lhs.v != 0 && rhs.v != 0);
                lhs = Val{r ? 1 : 0, false};
                continue;
            }
            Val rhs = binary(prec + 1);
            if (!err_.empty()) break;
            lhs = apply(op, lhs, rhs);
        }
        return lhs;
    }

    Val apply(const std::string& op, Val a, Val b) {
        const bool uns = a.u || b.u;
        const auto ua = static_cast<std::uint64_t>(a.v);
        const auto ub = static_cast<std::uint64_t>(b.v);
        auto mk = [](std::uint64_t v, bool u) { return Val{static_cast<std::int64_t>(v), u}; };
        if (op == "*") return mk(ua * ub, uns);
        if (op == "+") return mk(ua + ub, uns);
        if (op == "-") return mk(ua - ub, uns);
        if (op == "/" || op == "%") {
            if (b.v == 0) {
                if (skip_ == 0) fail("division by zero in #if");
                return Val{0, uns};
            }
            if (uns) return mk(op == "/" ? ua / ub : ua % ub, true);
            if (a.v == INT64_MIN && b.v == -1) return Val{op == "/" ? INT64_MIN : 0, false};
            return Val{op == "/" ? a.v / b.v : a.v % b.v, false};
        }
        if (op == "<<" || op == ">>") {
            bool left = op == "<<";
            std::uint64_t count = ub;
            if (!b.u && b.v < 0) {
                left = !left;
                count = static_cast<std::uint64_t>(-(b.v + 1)) + 1;
            }
            if (left) return count >= 64 ? Val{0, a.u} : mk(ua << count, a.u);
            if (a.u) return count >= 64 ? Val{0, true} : mk(ua >> count, true);
            if (count >= 64) return Val{a.v < 0 ? -1 : 0, false};
            return Val{a.v >> count, false}; // arithmetic shift
        }
        if (op == "&") return mk(ua & ub, uns);
        if (op == "|") return mk(ua | ub, uns);
        if (op == "^") return mk(ua ^ ub, uns);
        bool r = false;
        if (uns) {
            if (op == "<") r = ua < ub;
            else if (op == ">") r = ua > ub;
            else if (op == "<=") r = ua <= ub;
            else if (op == ">=") r = ua >= ub;
            else if (op == "==") r = ua == ub;
            else r = ua != ub;
        } else {
            if (op == "<") r = a.v < b.v;
            else if (op == ">") r = a.v > b.v;
            else if (op == "<=") r = a.v <= b.v;
            else if (op == ">=") r = a.v >= b.v;
            else if (op == "==") r = a.v == b.v;
            else r = a.v != b.v;
        }
        return Val{r ? 1 : 0, false};
    }

    Val unary() {
        DepthGuard guard(*this);
        if (tooDeep()) return {};
        const PTok* t = peek();
        if (!t) {
            fail("#if with no expression or operand expected");
            return {};
        }
        if (t->t->kind == TokKind::Punct) {
            const std::string& s = t->text();
            if (s == "+" || s == "-" || s == "!" || s == "~") {
                ++pos_;
                Val v = unary();
                if (s == "-") return Val{static_cast<std::int64_t>(0 - static_cast<std::uint64_t>(v.v)), v.u};
                if (s == "~") return Val{~v.v, v.u};
                if (s == "!") return Val{v.v == 0 ? 1 : 0, false};
                return v;
            }
            if (s == "(") {
                ++pos_;
                Val v = comma();
                if (!err_.empty()) return v;
                if (!peekPunct(")")) {
                    fail("missing ')' in expression");
                    return v;
                }
                ++pos_;
                return v;
            }
            fail("token '" + s + "' is not valid in preprocessor expressions");
            return {};
        }
        ++pos_;
        switch (t->t->kind) {
        case TokKind::Number: return number(t->text());
        case TokKind::CharLit: return character(t->text());
        case TokKind::Ident: {
            const std::string& s = t->text();
            if (s == "true") return Val{1, false};
            if (s == "false") return Val{0, false};
            if (s == "__has_attribute" || s == "__has_cpp_attribute" || s == "__has_builtin" || s == "__has_feature" ||
                s == "__has_extension") {
                skipBalancedParens();
                return Val{0, false};
            }
            return Val{0, false}; // identifiers that are not macros are 0
        }
        case TokKind::StringLit: fail("string literal in preprocessor expression"); return {};
        default: fail("unexpected end of expression"); return {};
        }
    }

    void skipBalancedParens() {
        if (!peekPunct("(")) return;
        int depth = 0;
        while (pos_ < toks_.size()) {
            if (toks_[pos_].isPunct("(")) ++depth;
            else if (toks_[pos_].isPunct(")") && --depth == 0) {
                ++pos_;
                return;
            }
            ++pos_;
        }
    }

    Val number(const std::string& raw) {
        std::string s;
        s.reserve(raw.size());
        for (char c : raw)
            if (c != '\'') s.push_back(c);
        std::size_t i = 0;
        unsigned base = 10;
        if (s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            i = 2;
        } else if (s.size() > 1 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
            base = 2;
            i = 2;
        } else if (s.size() > 1 && s[0] == '0') {
            base = 8;
            i = 1;
        }
        std::uint64_t value = 0;
        bool any = base == 8; // the leading 0 counts as a digit
        bool overflow = false;
        for (; i < s.size(); ++i) {
            const char c = s[i];
            unsigned d;
            if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
            else if (base == 16 && c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
            else if (base == 16 && c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
            else break;
            if (d >= base) {
                fail("invalid digit '" + std::string(1, c) + "' in constant '" + raw + "'");
                return {};
            }
            if (value > (UINT64_MAX - d) / base) overflow = true;
            value = value * base + d;
            any = true;
        }
        if (!any) {
            fail("invalid integer constant '" + raw + "'");
            return {};
        }
        bool uns = false;
        std::string suffix = s.substr(i);
        std::string lower;
        for (char c : suffix) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        // Accepted: u, l, ll, ul, ull, lu, llu (any order of u) and the MSVC i8/i16/i32/i64 forms.
        std::string rest = lower;
        auto eraseOnce = [&](const std::string& what) {
            const std::size_t p = rest.find(what);
            if (p == std::string::npos) return false;
            rest.erase(p, what.size());
            return true;
        };
        if (eraseOnce("u")) uns = true;
        if (!(eraseOnce("i64") || eraseOnce("i32") || eraseOnce("i16") || eraseOnce("i8") || eraseOnce("ll") ||
              eraseOnce("l")))
            (void)0;
        if (!rest.empty() || (base == 10 && (s.find('.') != std::string::npos))) {
            fail("invalid suffix or floating constant in preprocessor expression: '" + raw + "'");
            return {};
        }
        if (overflow) uns = true;
        if (value > static_cast<std::uint64_t>(INT64_MAX)) uns = true;
        return Val{static_cast<std::int64_t>(value), uns};
    }

    Val character(const std::string& text) {
        std::size_t i = 0;
        bool wide = false;
        while (i < text.size() && text[i] != '\'') {
            if (text[i] == 'L' || text[i] == 'u' || text[i] == 'U') wide = true;
            ++i;
        }
        if (i >= text.size()) {
            fail("malformed character constant");
            return {};
        }
        ++i;
        std::uint64_t value = 0;
        int count = 0;
        std::uint64_t last = 0;
        while (i < text.size() && text[i] != '\'') {
            std::uint64_t c;
            if (text[i] == '\\' && i + 1 < text.size()) {
                ++i;
                const char e = text[i++];
                switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'a': c = '\a'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'v': c = '\v'; break;
                case 'x': {
                    c = 0;
                    while (i < text.size() && std::isxdigit(static_cast<unsigned char>(text[i]))) {
                        c = c * 16 + static_cast<std::uint64_t>(std::isdigit(static_cast<unsigned char>(text[i]))
                                                                    ? text[i] - '0'
                                                                    : std::tolower(static_cast<unsigned char>(text[i])) - 'a' + 10);
                        ++i;
                    }
                    break;
                }
                default:
                    if (e >= '0' && e <= '7') {
                        c = static_cast<std::uint64_t>(e - '0');
                        for (int k = 0; k < 2 && i < text.size() && text[i] >= '0' && text[i] <= '7'; ++k)
                            c = c * 8 + static_cast<std::uint64_t>(text[i++] - '0');
                    } else {
                        c = static_cast<unsigned char>(e);
                    }
                }
            } else {
                c = static_cast<unsigned char>(text[i++]);
            }
            last = c;
            value = (value << 8) | (c & 0xff);
            ++count;
        }
        if (count == 0) {
            fail("empty character constant");
            return {};
        }
        if (wide || count > 1) return Val{static_cast<std::int64_t>(count == 1 ? last : value), false};
        return Val{static_cast<std::int64_t>(static_cast<signed char>(last & 0xff)), false}; // plain char is signed
    }

    const std::vector<PTok>& toks_;
    std::size_t pos_ = 0;
    int skip_ = 0;
    int depth_ = 0;
    std::string err_;
};

// ---------------------------------------------------------------------------------------------------------------------

class Impl {
public:
    Impl(const SourceProvider& source, const PreprocessOptions& options) : source_(source), options_(options) {}

    PreprocessResult run(std::string_view rootPath) {
        installBuiltins();
        for (const auto& [name, value] : options_.defines) definePredefined(name, value);

        const std::string root = normalizePath(rootPath);
        FileData* fd = root.empty() ? nullptr : loadFile(root);
        if (!fd) {
            addDiag(Diag::Severity::Error, "cannot open source file '" + std::string(rootPath) + "'", 0, 0, std::string(rootPath));
            finish(result_.tokens);
            return std::move(result_);
        }
        pushFrame(fd);

        auto& out = result_.tokens;
        out.reserve(1 << 16);
        for (;;) {
            PTok t = next();
            if (t.isEnd()) break;
            if (out.size() >= options_.maxOutputTokens) {
                addDiag(Diag::Severity::Error, "output token limit exceeded", t.file, t.line);
                result_.aborted = true;
                break;
            }
            Token o;
            o.kind = t.t->kind;
            o.text = t.t->text;
            o.file = t.file;
            o.line = t.line;
            o.atLineStart = (t.flags & kLineStart) != 0;
            o.spaceBefore = t.space();
            out.push_back(std::move(o));
        }
        finish(out);
        return std::move(result_);
    }

private:
    // ----- small helpers ---------------------------------------------------------------------------------------------

    void finish(std::vector<Token>& out) {
        Token end;
        end.kind = TokKind::End;
        end.atLineStart = true;
        if (!out.empty()) {
            end.file = out.back().file;
            end.line = out.back().line;
        }
        out.push_back(std::move(end));
        result_.files = files_;
        for (const auto& [name, m] : macros_) {
            if (m->builtin == Builtin::None) result_.finalMacros.emplace_back(name, renderMacro(*m));
        }
        std::sort(result_.finalMacros.begin(), result_.finalMacros.end());
    }

    void addDiag(Diag::Severity sev, std::string msg, std::uint32_t file, std::uint32_t line, std::string pathOverride = {}) {
        Diag d;
        d.severity = sev;
        d.message = std::move(msg);
        d.file = pathOverride.empty() ? (file < files_.size() ? files_[file] : std::string()) : std::move(pathOverride);
        d.line = line;
        result_.diags.push_back(std::move(d));
    }

    const Token* newToken(TokKind kind, std::string text) {
        Token& t = arena_.emplace_back();
        t.kind = kind;
        t.text = std::move(text);
        return &t;
    }

    PTok makeTok(const Token* t, std::uint32_t file, std::uint32_t line, std::uint8_t flags) {
        PTok p;
        p.t = t;
        p.file = file;
        p.line = line;
        p.flags = flags;
        return p;
    }

    static std::uint8_t flagsOf(const Token& t) {
        return static_cast<std::uint8_t>((t.spaceBefore ? kSpace : 0) | (t.atLineStart ? kLineStart : 0));
    }

    // ----- built-in and predefined macros ----------------------------------------------------------------------------

    void installBuiltin(const char* name, Builtin kind, bool funcLike) {
        auto& m = macroStore_.emplace_back();
        m.name = name;
        m.builtin = kind;
        m.funcLike = funcLike;
        if (funcLike) {
            m.params.push_back("x");
            m.variadic = true;
        }
        macros_[m.name] = &m;
    }

    void installBuiltins() {
        installBuiltin("__FILE__", Builtin::File, false);
        installBuiltin("__LINE__", Builtin::Line, false);
        installBuiltin("__COUNTER__", Builtin::Counter, false);
        installBuiltin("__INCLUDE_LEVEL__", Builtin::IncludeLevel, false);
        installBuiltin("_Pragma", Builtin::Pragma, true);
        installBuiltin("__has_include", Builtin::HasInclude, true);
        installBuiltin("__has_include_next", Builtin::HasInclude, true);
        installBuiltin("__has_attribute", Builtin::HasAttribute, true);
        installBuiltin("__has_cpp_attribute", Builtin::HasAttribute, true);
        installBuiltin("__has_builtin", Builtin::HasAttribute, true);
        installBuiltin("__has_feature", Builtin::HasAttribute, true);
        installBuiltin("__has_extension", Builtin::HasAttribute, true);
    }

    void definePredefined(const std::string& name, const std::string& value) {
        auto holder = std::make_unique<std::vector<Token>>(lexFragment(name + " " + value, 0, 0));
        const auto& toks = *holder;
        predefTokens_.push_back(std::move(holder));
        defineMacro(toks.data(), toks.data() + toks.size(), 0, 0);
    }

    // ----- files -----------------------------------------------------------------------------------------------------

    static std::string detectGuard(const std::vector<Token>& toks) {
        auto isHash = [&](std::size_t i) { return toks[i].atLineStart && toks[i].kind == TokKind::Punct && toks[i].text == "#"; };
        if (toks.size() < 4 || !isHash(0) || toks[1].kind != TokKind::Ident) return {};
        std::string guard;
        std::size_t afterLine;
        if (toks[1].text == "ifndef" && toks[2].kind == TokKind::Ident) {
            guard = toks[2].text;
            afterLine = 3;
        } else if (toks[1].text == "if" && toks.size() > 6 && toks[2].isPunct("!") && toks[3].isIdent("defined")) {
            if (toks[4].isPunct("(") && toks[5].kind == TokKind::Ident && toks[6].isPunct(")")) {
                guard = toks[5].text;
                afterLine = 7;
            } else if (toks[4].kind == TokKind::Ident) {
                guard = toks[4].text;
                afterLine = 5;
            } else {
                return {};
            }
        } else {
            return {};
        }
        if (afterLine >= toks.size() || (!toks[afterLine].atLineStart && toks[afterLine].kind != TokKind::End)) return {};
        int depth = 0;
        for (std::size_t i = 0; i < toks.size(); ++i) {
            if (toks[i].kind == TokKind::End) return {};
            if (!isHash(i) || i + 1 >= toks.size() || toks[i + 1].kind != TokKind::Ident) continue;
            const std::string& d = toks[i + 1].text;
            if (d == "if" || d == "ifdef" || d == "ifndef") {
                ++depth;
            } else if (d == "endif" && --depth == 0) {
                std::size_t j = i + 2;
                while (toks[j].kind != TokKind::End && !toks[j].atLineStart) ++j;
                return toks[j].kind == TokKind::End ? guard : std::string();
            }
        }
        return {};
    }

    // Reads and lexes a file once. nullptr when it does not exist.
    FileData* loadFile(const std::string& path) {
        auto it = fileCache_.find(path);
        if (it != fileCache_.end()) return it->second;
        std::optional<std::string> content = source_.read(path);
        if (!content) {
            fileCache_.emplace(path, nullptr);
            return nullptr;
        }
        auto& fd = fileStore_.emplace_back(std::make_unique<FileData>());
        fd->path = path;
        fd->index = static_cast<std::uint32_t>(files_.size());
        files_.push_back(path);
        std::string_view text = *content;
        if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
        fd->tokens = lex(text, fd->index);
        fd->guard = detectGuard(fd->tokens);
        fileCache_.emplace(path, fd.get());
        return fd.get();
    }

    FileData* locate(const std::string& name, bool quoted, std::uint32_t fromFile) {
        if (name.empty()) return nullptr;
        if (quoted) {
            const std::string p = joinPath(dirName(files_[fromFile]), name);
            if (!p.empty())
                if (FileData* fd = loadFile(p)) return fd;
        }
        for (const std::string& dir : options_.includeDirs) {
            const std::string p = joinPath(dir, name);
            if (p.empty()) continue;
            if (FileData* fd = loadFile(p)) return fd;
        }
        return nullptr;
    }

    void pushFrame(FileData* fd) {
        FileFrame f;
        f.fd = fd;
        f.condBase = conds_.size();
        frames_.push_back(f);
    }

    void popFrame() {
        const FileFrame& f = frames_.back();
        while (conds_.size() > f.condBase) {
            addDiag(Diag::Severity::Error, "unterminated conditional directive", conds_.back().file, conds_.back().line);
            conds_.pop_back();
        }
        updateSkipping();
        frames_.pop_back();
    }

    void updateSkipping() { skipping_ = !conds_.empty() && !conds_.back().active; }

    // ----- raw token source: file stack + directives -----------------------------------------------------------------

    bool rawNext(PTok& out) {
        for (;;) {
            if (frames_.empty() || aborted_) return false;
            FileFrame& f = frames_.back();
            const Token& t = f.fd->tokens[f.pos];
            if (t.kind == TokKind::End) {
                popFrame();
                continue;
            }
            if (t.atLineStart && t.kind == TokKind::Punct && t.text == "#") {
                handleDirective();
                continue;
            }
            ++f.pos;
            if (skipping_) continue;
            out = makeTok(&t, f.fd->index, t.line, flagsOf(t));
            return true;
        }
    }

    // ----- directives ------------------------------------------------------------------------------------------------

    void handleDirective() {
        FileFrame& f = frames_.back();
        FileData* fd = f.fd;
        const std::vector<Token>& toks = fd->tokens;
        const Token* b = &toks[f.pos + 1];
        const Token* e = b;
        while (e->kind != TokKind::End && !e->atLineStart) ++e;
        const std::uint32_t line = toks[f.pos].line;
        f.pos = static_cast<std::size_t>(e - toks.data()); // f may be invalidated below, so advance first
        if (b == e) return;                                // null directive
        if (b->kind != TokKind::Ident) return;             // "# 12 "file"" style line markers
        const std::string& name = b->text;
        ++b;
        dirFile_ = fd->index;
        dirLine_ = line;

        if (name == "if" || name == "ifdef" || name == "ifndef") {
            Cond c;
            c.file = fd->index;
            c.line = line;
            c.parentActive = !skipping_;
            if (!skipping_) {
                bool v;
                if (name == "if") v = evalCondition(b, e);
                else v = isDefinedDirective(b, e, name == "ifdef", name);
                c.active = v;
                c.taken = v;
            } else {
                c.taken = true; // nothing in a skipped group can become active
            }
            conds_.push_back(c);
            updateSkipping();
            return;
        }
        if (name == "elif") {
            if (conds_.size() <= frames_.back().condBase) {
                addDiag(Diag::Severity::Error, "#elif without #if", fd->index, line);
                return;
            }
            Cond& c = conds_.back();
            if (c.seenElse) addDiag(Diag::Severity::Error, "#elif after #else", fd->index, line);
            if (c.parentActive && !c.taken) {
                skipping_ = false; // the expression is evaluated in the enclosing (active) context
                const bool v = evalCondition(b, e);
                Cond& cc = conds_.back();
                cc.active = v;
                cc.taken = v;
            } else {
                c.active = false;
            }
            updateSkipping();
            return;
        }
        if (name == "else") {
            if (conds_.size() <= frames_.back().condBase) {
                addDiag(Diag::Severity::Error, "#else without #if", fd->index, line);
                return;
            }
            Cond& c = conds_.back();
            if (c.seenElse) addDiag(Diag::Severity::Error, "#else after #else", fd->index, line);
            c.seenElse = true;
            c.active = c.parentActive && !c.taken;
            c.taken = true;
            updateSkipping();
            return;
        }
        if (name == "endif") {
            if (conds_.size() <= frames_.back().condBase) {
                addDiag(Diag::Severity::Error, "#endif without #if", fd->index, line);
                return;
            }
            conds_.pop_back();
            updateSkipping();
            return;
        }
        if (skipping_) return;

        if (name == "define") {
            defineMacro(b, e, fd->index, line);
        } else if (name == "undef") {
            if (b == e || b->kind != TokKind::Ident) {
                addDiag(Diag::Severity::Error, "macro name missing in #undef", fd->index, line);
            } else {
                macros_.erase(b->text);
            }
        } else if (name == "include" || name == "include_next" || name == "import") {
            doInclude(b, e, fd->index, line, name == "import");
        } else if (name == "error") {
            addDiag(Diag::Severity::Error, "#error " + joinSpelling(b, e), fd->index, line);
        } else if (name == "warning") {
            addDiag(Diag::Severity::Warning, "#warning " + joinSpelling(b, e), fd->index, line);
        } else if (name == "pragma") {
            doPragma(b, e, fd->index, line);
        } else if (name == "line" || name == "ident" || name == "sccs" || name == "assert" || name == "unassert") {
            // ignored
        } else {
            addDiag(Diag::Severity::Warning, "invalid preprocessing directive #" + name, fd->index, line);
        }
    }

    static std::string joinSpelling(const Token* b, const Token* e) {
        std::string s;
        for (const Token* p = b; p != e; ++p) {
            if (p != b && p->spaceBefore) s += ' ';
            s += p->text;
        }
        return s;
    }

    bool isDefinedDirective(const Token* b, const Token* e, bool wantDefined, const std::string& dir) {
        if (b == e || b->kind != TokKind::Ident) {
            addDiag(Diag::Severity::Error, "no macro name given in #" + dir + " directive", dirFile_, dirLine_);
            return false;
        }
        const bool def = macros_.find(b->text) != macros_.end();
        return def == wantDefined;
    }

    void doPragma(const Token* b, const Token* e, std::uint32_t file, std::uint32_t line) {
        if (b != e && b->kind == TokKind::Ident) {
            if (b->text == "once") {
                markOnce(file);
                return;
            }
            if (b->text == "push_macro" || b->text == "pop_macro") {
                pragmaMacroStack(b, e);
                return;
            }
        }
        PragmaRecord r;
        r.text = joinSpelling(b, e);
        r.file = file;
        r.line = line;
        r.tokenIndex = result_.tokens.size();
        result_.pragmas.push_back(std::move(r));
    }

    void pragmaMacroStack(const Token* b, const Token* e) {
        const bool push = b->text == "push_macro";
        std::string name;
        for (const Token* p = b + 1; p != e; ++p)
            if (p->kind == TokKind::StringLit && p->text.size() >= 2) name = p->text.substr(1, p->text.size() - 2);
        if (name.empty()) return;
        auto& stack = macroStack_[name];
        if (push) {
            auto it = macros_.find(name);
            stack.push_back(it == macros_.end() ? nullptr : it->second);
        } else if (!stack.empty()) {
            Macro* m = stack.back();
            stack.pop_back();
            if (m) macros_[name] = m;
            else macros_.erase(name);
        }
    }

    void markOnce(std::uint32_t fileIndex) {
        if (onceFiles_.size() <= fileIndex) onceFiles_.resize(fileIndex + 1, false);
        onceFiles_[fileIndex] = true;
    }
    bool isOnce(std::uint32_t fileIndex) const { return fileIndex < onceFiles_.size() && onceFiles_[fileIndex]; }

    // ----- #include --------------------------------------------------------------------------------------------------

    // Extracts the file name of an include specification: "name" or <name>.
    bool parseIncludeSpec(const std::vector<PTok>& toks, std::string& name, bool& quoted) {
        if (toks.empty()) return false;
        if (toks[0].t->kind == TokKind::StringLit && toks[0].text().size() >= 2 && toks[0].text()[0] == '"') {
            name = toks[0].text().substr(1, toks[0].text().size() - 2);
            quoted = true;
            return true;
        }
        if (toks[0].isPunct("<")) {
            name.clear();
            for (std::size_t i = 1; i < toks.size(); ++i) {
                if (toks[i].isPunct(">")) {
                    quoted = false;
                    return true;
                }
                if (i > 1 && toks[i].space()) name += ' ';
                name += toks[i].text();
            }
        }
        return false;
    }

    std::vector<PTok> directiveTokens(const Token* b, const Token* e) {
        std::vector<PTok> v;
        v.reserve(static_cast<std::size_t>(e - b));
        for (const Token* p = b; p != e; ++p) v.push_back(makeTok(p, dirFile_, p->line, flagsOf(*p)));
        return v;
    }

    // Macro-expands a directive line (used by computed #include and #if).
    std::vector<PTok> expandLine(std::vector<PTok> toks) {
        std::vector<PTok> out;
        PTok endTok;
        toks.push_back(endTok);
        Context c;
        c.toks = std::move(toks);
        ctx_.push_back(std::move(c));
        const std::size_t depth = ctx_.size();
        for (;;) {
            PTok t = next();
            if (t.isEnd()) break;
            out.push_back(t);
        }
        while (ctx_.size() >= depth) popContext();
        return out;
    }

    void doInclude(const Token* b, const Token* e, std::uint32_t fromFile, std::uint32_t line, bool once) {
        std::vector<PTok> toks = directiveTokens(b, e);
        std::string name;
        bool quoted = false;
        if (toks.empty() || !(toks[0].t->kind == TokKind::StringLit || toks[0].isPunct("<")))
            toks = expandLine(std::move(toks));
        if (!parseIncludeSpec(toks, name, quoted)) {
            addDiag(Diag::Severity::Error, "#include expects \"FILENAME\" or <FILENAME>", fromFile, line);
            return;
        }
        FileData* fd = locate(name, quoted, fromFile);
        if (!fd) {
            if (quoted || !options_.silentSystemIncludes)
                addDiag(Diag::Severity::Warning, std::string("cannot find include file ") + (quoted ? "\"" : "<") + name + (quoted ? "\"" : ">"), fromFile, line);
            return;
        }
        if (once) markOnce(fd->index);
        if (isOnce(fd->index)) return;
        if (!fd->guard.empty() && macros_.find(fd->guard) != macros_.end()) return;
        if (static_cast<int>(frames_.size()) >= options_.maxIncludeDepth) {
            addDiag(Diag::Severity::Error, "#include nested too deeply (limit " + std::to_string(options_.maxIncludeDepth) + "): " + name, fromFile, line);
            return;
        }
        pushFrame(fd);
    }

    // ----- #if -------------------------------------------------------------------------------------------------------

    bool evalCondition(const Token* b, const Token* e) {
        if (b == e) {
            addDiag(Diag::Severity::Error, "#if with no expression", dirFile_, dirLine_);
            return false;
        }
        std::vector<PTok> line = directiveTokens(b, e);
        line.push_back(PTok{});
        Context c;
        c.toks = std::move(line);
        ctx_.push_back(std::move(c));
        const std::size_t depth = ctx_.size();
        std::vector<PTok> expr;
        for (;;) {
            PTok t = next();
            if (t.isEnd()) break;
            if (t.isIdent("defined")) {
                PTok p = fetch();
                const bool paren = p.isPunct("(");
                if (paren) p = fetch();
                bool ok = p.isIdent();
                const bool def = ok && macros_.find(p.text()) != macros_.end();
                if (paren) {
                    PTok close = fetch();
                    if (!close.isPunct(")")) ok = false;
                }
                if (!ok) {
                    addDiag(Diag::Severity::Error, "operator \"defined\" requires an identifier", dirFile_, dirLine_);
                    expr.clear();
                    break;
                }
                expr.push_back(makeTok(newToken(TokKind::Number, def ? "1" : "0"), dirFile_, dirLine_, 0));
            } else if (t.isIdent("__has_include") || t.isIdent("__has_include_next")) {
                expr.push_back(makeTok(newToken(TokKind::Number, evalHasInclude() ? "1" : "0"), dirFile_, dirLine_, 0));
            } else {
                expr.push_back(t);
            }
        }
        while (ctx_.size() >= depth) popContext();
        if (expr.empty()) return false;
        ExprParser parser(expr);
        Val v;
        if (!parser.parse(v)) {
            addDiag(Diag::Severity::Error, "#if: " + parser.error(), dirFile_, dirLine_);
            return false;
        }
        return v.v != 0;
    }

    bool evalHasInclude() {
        PTok p = fetch();
        if (!p.isPunct("(")) {
            addDiag(Diag::Severity::Error, "missing '(' after __has_include", dirFile_, dirLine_);
            pushBack(p);
            return false;
        }
        std::vector<PTok> spec;
        int depth = 0;
        for (;;) {
            PTok t = fetch();
            if (t.isEnd()) {
                addDiag(Diag::Severity::Error, "missing ')' after __has_include", dirFile_, dirLine_);
                break;
            }
            if (t.isPunct("(")) ++depth;
            if (t.isPunct(")")) {
                if (depth == 0) break;
                --depth;
            }
            spec.push_back(t);
        }
        std::string name;
        bool quoted = false;
        if (!parseIncludeSpec(spec, name, quoted)) {
            spec = expandLine(std::move(spec));
            if (!parseIncludeSpec(spec, name, quoted)) return false;
        }
        return locate(name, quoted, dirFile_) != nullptr;
    }

    // ----- #define ---------------------------------------------------------------------------------------------------

    void defineMacro(const Token* b, const Token* e, std::uint32_t file, std::uint32_t line) {
        if (b == e || b->kind != TokKind::Ident) {
            addDiag(Diag::Severity::Error, "macro names must be identifiers", file, line);
            return;
        }
        Macro m;
        m.name = b->text;
        ++b;
        auto err = [&](const std::string& msg) { addDiag(Diag::Severity::Error, msg + " in definition of macro '" + m.name + "'", file, line); };

        if (b != e && b->isPunct("(") && !b->spaceBefore) {
            m.funcLike = true;
            ++b;
            bool first = true;
            for (;;) {
                if (b == e) return err("missing ')' in parameter list");
                if (first && b->isPunct(")")) {
                    ++b;
                    break;
                }
                first = false;
                if (b->isPunct("...")) {
                    m.variadic = true;
                    m.params.push_back("__VA_ARGS__");
                    ++b;
                    if (b == e || !b->isPunct(")")) return err("missing ')' after '...'");
                    ++b;
                    break;
                }
                if (b->kind != TokKind::Ident) return err("invalid parameter");
                if (std::find(m.params.begin(), m.params.end(), b->text) != m.params.end()) return err("duplicate parameter '" + b->text + "'");
                m.params.push_back(b->text);
                ++b;
                if (b != e && b->isPunct("...")) {
                    m.variadic = true;
                    ++b;
                    if (b == e || !b->isPunct(")")) return err("missing ')' after '...'");
                    ++b;
                    break;
                }
                if (b == e) return err("missing ')' in parameter list");
                if (b->isPunct(",")) {
                    ++b;
                    continue;
                }
                if (b->isPunct(")")) {
                    ++b;
                    break;
                }
                return err("invalid parameter list");
            }
        }

        auto paramIndex = [&](const std::string& s) -> int {
            if (!m.funcLike) return -1;
            for (std::size_t i = 0; i < m.params.size(); ++i)
                if (m.params[i] == s) return static_cast<int>(i);
            return -1;
        };

        std::size_t vaOpen = SIZE_MAX;
        int vaDepth = 0;
        for (const Token* p = b; p != e; ++p) {
            BodyItem it;
            it.space = p->spaceBefore;
            if (p->kind == TokKind::Punct && p->text == "##") {
                it.kind = BodyItem::Kind::Paste;
            } else if (m.funcLike && p->kind == TokKind::Punct && p->text == "#" && p + 1 != e && (p + 1)->kind == TokKind::Ident &&
                       paramIndex((p + 1)->text) >= 0) {
                it.kind = BodyItem::Kind::Stringize;
                it.index = static_cast<std::uint32_t>(paramIndex((p + 1)->text));
                ++p;
            } else if (p->kind == TokKind::Ident && paramIndex(p->text) >= 0) {
                it.kind = BodyItem::Kind::Param;
                it.index = static_cast<std::uint32_t>(paramIndex(p->text));
            } else if (m.variadic && vaOpen == SIZE_MAX && p->kind == TokKind::Ident && p->text == "__VA_OPT__" && p + 1 != e && (p + 1)->isPunct("(")) {
                it.kind = BodyItem::Kind::VaOpen;
                vaOpen = m.items.size();
                vaDepth = 0;
                ++p;
            } else {
                it.kind = BodyItem::Kind::Tok;
                it.tok = *p;
                if (vaOpen != SIZE_MAX) {
                    if (p->isPunct("(")) {
                        ++vaDepth;
                    } else if (p->isPunct(")")) {
                        if (vaDepth == 0) {
                            it.kind = BodyItem::Kind::VaClose;
                            m.items[vaOpen].index = static_cast<std::uint32_t>(m.items.size());
                            vaOpen = SIZE_MAX;
                        } else {
                            --vaDepth;
                        }
                    }
                }
            }
            // `a ## ## b` behaves like `a ## b` (GCC)
            if (it.kind == BodyItem::Kind::Paste && !m.items.empty() && m.items.back().kind == BodyItem::Kind::Paste) continue;
            m.items.push_back(std::move(it));
        }
        if (vaOpen != SIZE_MAX) return err("unterminated __VA_OPT__");
        if (!m.items.empty() && (m.items.front().kind == BodyItem::Kind::Paste || m.items.back().kind == BodyItem::Kind::Paste))
            return err("'##' cannot appear at either end of a macro expansion");

        auto it = macros_.find(m.name);
        if (it != macros_.end()) {
            if (sameDefinition(*it->second, m)) return;
            if (it->second->builtin == Builtin::None)
                addDiag(Diag::Severity::Warning, "'" + m.name + "' redefined", file, line);
        }
        macroStore_.push_back(std::move(m));
        Macro* stored = &macroStore_.back();
        auto slot = macros_.find(stored->name);
        if (slot != macros_.end()) slot->second = stored;
        else macros_.emplace(stored->name, stored);
    }

    // ----- token pulling and macro expansion -------------------------------------------------------------------------

    void popContext() {
        if (ctx_.back().macro) ctx_.back().macro->disabled = false;
        ctx_.pop_back();
    }

    void pushBack(const PTok& t) {
        if (t.isEnd()) return;
        Context c;
        c.toks.push_back(t);
        ctx_.push_back(std::move(c));
    }

    // Next token without macro expansion. End is sticky.
    PTok fetch() {
        for (;;) {
            if (aborted_) return PTok{};
            if (!ctx_.empty()) {
                Context& c = ctx_.back();
                if (c.pos < c.toks.size()) {
                    const PTok& t = c.toks[c.pos];
                    if (!t.isEnd()) ++c.pos;
                    return t;
                }
                popContext();
                continue;
            }
            PTok t;
            if (rawNext(t)) return t;
            return PTok{};
        }
    }

    PTok next() {
        for (;;) {
            PTok t = fetch();
            if (t.t->kind != TokKind::Ident || (t.flags & kNoExpand)) return t;
            auto it = macros_.find(t.t->text);
            if (it == macros_.end()) return t;
            Macro* m = it->second;
            if (m->disabled) {
                t.flags |= kNoExpand;
                return t;
            }
            if (expandMacro(*m, t)) continue;
            return t;
        }
    }

    void abort(const std::string& why, const PTok& at) {
        if (aborted_) return;
        aborted_ = true;
        result_.aborted = true;
        addDiag(Diag::Severity::Error, why, at.file, at.line);
    }

    bool expandMacro(Macro& m, const PTok& name) {
        if (m.builtin != Builtin::None) return expandBuiltin(m, name);
        if (ctx_.size() >= static_cast<std::size_t>(options_.maxExpansionDepth)) {
            if (!depthReported_) {
                depthReported_ = true;
                addDiag(Diag::Severity::Error, "macro expansion depth limit reached expanding '" + m.name + "'", name.file, name.line);
            }
            return false;
        }
        if (++expansions_ > kMaxExpansions) {
            abort("macro expansion budget exceeded", name);
            return false;
        }
        std::vector<Arg> args;
        if (m.funcLike && !collectArgs(m, name, args)) return false;

        Context c;
        substitute(m, name, args, c.toks);
        produced_ += c.toks.size();
        if (produced_ > options_.maxOutputTokens * 2) {
            abort("macro expansion produced too many tokens", name);
            return false;
        }
        // Position information: everything produced by this invocation belongs to the outermost invocation.
        for (std::size_t i = 0; i < c.toks.size(); ++i) {
            PTok& t = c.toks[i];
            t.file = name.file;
            t.line = name.line;
            t.flags = static_cast<std::uint8_t>(t.flags & ~kLineStart);
        }
        if (!c.toks.empty()) {
            c.toks[0].flags = static_cast<std::uint8_t>((c.toks[0].flags & ~kSpace) | (name.flags & kSpace));
        }
        c.macro = &m;
        m.disabled = true;
        ctx_.push_back(std::move(c));
        return true;
    }

    bool expandBuiltin(Macro& m, const PTok& name) {
        std::vector<PTok> out;
        switch (m.builtin) {
        case Builtin::File: {
            std::string s = "\"";
            for (char ch : files_[name.file]) {
                if (ch == '\\' || ch == '"') s += '\\';
                s += ch;
            }
            s += '"';
            out.push_back(makeTok(newToken(TokKind::StringLit, std::move(s)), name.file, name.line, name.flags & kSpace));
            break;
        }
        case Builtin::Line:
            out.push_back(makeTok(newToken(TokKind::Number, std::to_string(name.line)), name.file, name.line, name.flags & kSpace));
            break;
        case Builtin::Counter:
            out.push_back(makeTok(newToken(TokKind::Number, std::to_string(counter_++)), name.file, name.line, name.flags & kSpace));
            break;
        case Builtin::IncludeLevel:
            out.push_back(makeTok(newToken(TokKind::Number, std::to_string(frames_.empty() ? 0 : frames_.size() - 1)), name.file, name.line, name.flags & kSpace));
            break;
        case Builtin::Pragma:
        case Builtin::HasAttribute: {
            // _Pragma("...") disappears; __has_*(x) outside #if is left alone unless called like a function.
            PTok p = fetch();
            if (!p.isPunct("(")) {
                pushBack(p);
                return false;
            }
            int depth = 0;
            for (;;) {
                PTok t = fetch();
                if (t.isEnd()) break;
                if (t.isPunct("(")) ++depth;
                if (t.isPunct(")") && depth-- == 0) break;
            }
            if (m.builtin == Builtin::HasAttribute) out.push_back(makeTok(newToken(TokKind::Number, "0"), name.file, name.line, name.flags & kSpace));
            break;
        }
        case Builtin::HasInclude:
        case Builtin::None: return false;
        }
        Context c;
        c.toks = std::move(out);
        ctx_.push_back(std::move(c));
        return true;
    }

    // Reads '(' and the arguments of a function-like macro invocation. Returns false when `name` is not an
    // invocation (no '(') or the invocation is malformed; the consumed tokens are then pushed back / reported.
    bool collectArgs(Macro& m, const PTok& name, std::vector<Arg>& args) {
        PTok open = fetch();
        if (!open.isPunct("(")) {
            pushBack(open);
            return false;
        }
        std::vector<PTok> flat; // everything after '(' in case we have to give up
        flat.push_back(open);
        const std::size_t np = m.params.size();
        Arg cur;
        int depth = 0;
        for (;;) {
            PTok t = fetch();
            if (t.isEnd()) {
                addDiag(Diag::Severity::Error, "unterminated argument list invoking macro '" + m.name + "'", name.file, name.line);
                restore(flat);
                return false;
            }
            flat.push_back(t);
            if (t.isPunct("(")) {
                ++depth;
            } else if (t.isPunct(")")) {
                if (depth == 0) break;
                --depth;
            } else if (t.isPunct(",") && depth == 0 && !(m.variadic && args.size() + 1 >= np)) {
                args.push_back(std::move(cur));
                cur = Arg{};
                continue;
            }
            cur.raw.push_back(t);
        }
        args.push_back(std::move(cur));
        if (np == 0 && args.size() == 1 && args[0].raw.empty()) args.clear();
        if (m.variadic && args.size() + 1 == np) {
            args.emplace_back(); // variadic part omitted entirely
            args.back().omitted = true;
        }
        if (args.size() != np) {
            addDiag(Diag::Severity::Error,
                    "macro '" + m.name + "' passed " + std::to_string(args.size()) + " arguments, but takes " + (m.variadic ? "at least " : "just ") +
                        std::to_string(m.variadic ? np - 1 : np),
                    name.file, name.line);
            restore(flat);
            return false;
        }
        return true;
    }

    void restore(const std::vector<PTok>& flat) {
        Context c;
        c.toks = flat;
        ctx_.push_back(std::move(c));
    }

    void expandArg(Arg& a) {
        if (a.done) return;
        a.done = true;
        Context c;
        c.toks = a.raw;
        c.toks.push_back(PTok{});
        ctx_.push_back(std::move(c));
        const std::size_t depth = ctx_.size();
        for (;;) {
            PTok t = next();
            if (t.isEnd()) break;
            a.expanded.push_back(t);
        }
        while (ctx_.size() >= depth) popContext();
    }

    PTok stringize(const std::vector<PTok>& raw, const PTok& at) {
        std::string s = "\"";
        bool first = true;
        for (const PTok& t : raw) {
            if (!first && t.space()) s += ' ';
            first = false;
            const bool escape = t.t->kind == TokKind::StringLit || t.t->kind == TokKind::CharLit;
            for (char ch : t.text()) {
                if (escape && (ch == '\\' || ch == '"')) s += '\\';
                s += ch;
            }
        }
        s += '"';
        return makeTok(newToken(TokKind::StringLit, std::move(s)), at.file, at.line, 0);
    }

    // Concatenates two tokens; false when the result is not a single valid token.
    bool pasteTokens(const PTok& a, const PTok& b, PTok& result) {
        const TokKind ka = a.t->kind;
        const TokKind kb = b.t->kind;
        std::string text = a.text() + b.text();
        TokKind kind;
        if (ka == TokKind::Ident && (kb == TokKind::Ident || kb == TokKind::Number)) {
            kind = TokKind::Ident;
        } else if (ka == TokKind::Number && (kb == TokKind::Ident || kb == TokKind::Number)) {
            kind = TokKind::Number;
        } else {
            std::vector<Token> lexed = lexFragment(text, 0, 1);
            if (lexed.size() != 1 || lexed[0].text != text) return false;
            kind = lexed[0].kind;
        }
        result = makeTok(newToken(kind, std::move(text)), a.file, a.line, a.flags & kSpace);
        return true;
    }

    void substitute(Macro& m, const PTok& name, std::vector<Arg>& args, std::vector<PTok>& out) {
        const auto& items = m.items;
        const std::size_t n = items.size();
        const bool vaEmpty = m.variadic && !args.empty() && args.back().raw.empty();
        const std::size_t lastParam = m.params.empty() ? 0 : m.params.size() - 1;

        PTok placemarker;
        placemarker.t = &kPlacemarker;

        auto bodyTok = [&](const BodyItem& it) { return makeTok(&it.tok, name.file, name.line, it.space ? kSpace : 0); };

        auto appendItem = [&](std::size_t i, std::vector<PTok>& dst, bool forPaste) {
            const BodyItem& it = items[i];
            switch (it.kind) {
            case BodyItem::Kind::Tok: dst.push_back(bodyTok(it)); break;
            case BodyItem::Kind::Stringize:
                dst.push_back(stringize(args[it.index].raw, name));
                dst.back().flags = it.space ? kSpace : 0;
                break;
            case BodyItem::Kind::Param: {
                Arg& a = args[it.index];
                const std::size_t before = dst.size();
                if (forPaste) {
                    if (a.raw.empty()) dst.push_back(placemarker);
                    else dst.insert(dst.end(), a.raw.begin(), a.raw.end());
                } else {
                    expandArg(a);
                    dst.insert(dst.end(), a.expanded.begin(), a.expanded.end());
                }
                if (dst.size() > before) dst[before].flags = static_cast<std::uint8_t>((dst[before].flags & ~kSpace) | (it.space ? kSpace : 0));
                break;
            }
            default: break;
            }
        };

        auto isPasteAt = [&](std::size_t i) { return i + 1 < n && items[i + 1].kind == BodyItem::Kind::Paste && i + 2 < n; };

        std::vector<PTok> tmp;
        for (std::size_t i = 0; i < n;) {
            const BodyItem& it = items[i];
            if (it.kind == BodyItem::Kind::VaOpen) {
                i = vaEmpty ? it.index + 1 : i + 1;
                continue;
            }
            if (it.kind == BodyItem::Kind::VaClose || it.kind == BodyItem::Kind::Paste) {
                ++i;
                continue;
            }
            appendItem(i, out, isPasteAt(i));
            std::size_t j = i;
            while (isPasteAt(j)) {
                const std::size_t r = j + 2;
                const bool gnuComma = items[j].kind == BodyItem::Kind::Tok && items[j].tok.isPunct(",") && items[r].kind == BodyItem::Kind::Param &&
                                      m.variadic && items[r].index == lastParam;
                if (gnuComma) {
                    if (args[lastParam].omitted) {
                        out.pop_back(); // `, ## __VA_ARGS__` without any variable argument drops the comma (GCC rule)
                    } else {
                        const auto& raw = args[lastParam].raw;
                        out.insert(out.end(), raw.begin(), raw.end());
                    }
                } else {
                    tmp.clear();
                    appendItem(r, tmp, true);
                    if (tmp.empty()) tmp.push_back(placemarker);
                    pasteInto(out, tmp);
                }
                j = r;
            }
            i = j + 1;
        }
        out.erase(std::remove_if(out.begin(), out.end(), [](const PTok& t) { return t.isPlacemarker(); }), out.end());
    }

    void pasteInto(std::vector<PTok>& out, const std::vector<PTok>& rhs) {
        if (out.empty()) {
            out.insert(out.end(), rhs.begin(), rhs.end());
            return;
        }
        PTok& lhs = out.back();
        if (lhs.isPlacemarker()) {
            lhs = rhs[0];
        } else if (!rhs[0].isPlacemarker()) {
            PTok joined;
            if (pasteTokens(lhs, rhs[0], joined)) {
                lhs = joined;
            } else {
                addDiag(Diag::Severity::Warning, "pasting \"" + lhs.text() + "\" and \"" + rhs[0].text() + "\" does not give a valid preprocessing token", lhs.file, lhs.line);
                out.push_back(rhs[0]);
            }
        }
        out.insert(out.end(), rhs.begin() + 1, rhs.end());
    }

    // ----- state -----------------------------------------------------------------------------------------------------

    const SourceProvider& source_;
    const PreprocessOptions& options_;
    PreprocessResult result_;

    std::vector<std::string> files_;
    std::vector<std::unique_ptr<FileData>> fileStore_;
    StrMap<FileData*> fileCache_;
    std::vector<bool> onceFiles_;
    std::vector<FileFrame> frames_;
    std::vector<Cond> conds_;
    bool skipping_ = false;

    std::deque<Macro> macroStore_;
    StrMap<Macro*> macros_;
    StrMap<std::vector<Macro*>> macroStack_;
    std::vector<std::unique_ptr<std::vector<Token>>> predefTokens_;
    std::deque<Token> arena_;

    std::vector<Context> ctx_;
    std::uint32_t dirFile_ = 0;
    std::uint32_t dirLine_ = 0;
    std::size_t counter_ = 0;
    std::size_t expansions_ = 0;
    std::size_t produced_ = 0;
    bool aborted_ = false;
    bool depthReported_ = false;
};

} // namespace

Preprocessor::Preprocessor(const SourceProvider& source, PreprocessOptions options)
    : source_(source), options_(std::move(options)) {}

PreprocessResult Preprocessor::run(std::string_view path) {
    try {
        Impl impl(source_, options_);
        return impl.run(path);
    } catch (const std::exception& ex) {
        PreprocessResult r;
        Diag d;
        d.severity = Diag::Severity::Error;
        d.message = std::string("internal preprocessor failure: ") + ex.what();
        r.diags.push_back(std::move(d));
        r.aborted = true;
        r.tokens.emplace_back();
        return r;
    }
}

} // namespace qstate::cpp
