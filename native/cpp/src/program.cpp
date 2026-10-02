// Program: glue between the public API and the parser / evaluator / layout engine; token arena and symbols.
#include "qstate/cpp/program.h"

#include <algorithm>
#include <climits>

#include "program_impl.h"

namespace qstate::cpp {

void parseDeclarations(Program::Impl& impl, std::uint32_t begin, std::uint32_t end);

namespace {
constexpr std::uint32_t kNpos = UINT32_MAX;

const char* const kSymTexts[] = {
#define QSTATE_SYM_TEXT(name, text) text,
    QSTATE_SYMS(QSTATE_SYM_TEXT)
#undef QSTATE_SYM_TEXT
};
} // namespace

Program::Impl::Impl(ProgramOptions o) : opts(std::move(o)) {
    for (const char* t : kSymTexts) {
        Sym id = static_cast<Sym>(symText.size());
        symText.emplace_back(t);
        symMap.emplace(t, id);
    }
    decls.emplace_back();
    global = &decls.back();
    global->kind = DeclKind::Namespace;
    global->defined = true;
    initBuiltins();
}

Sym Program::Impl::intern(std::string_view s) {
    auto it = symMap.find(std::string(s));
    if (it != symMap.end()) return it->second;
    Sym id = static_cast<Sym>(symText.size());
    symText.emplace_back(s);
    symMap.emplace(std::string(s), id);
    return id;
}

TokRange Program::Impl::appendTokens(std::vector<Token>&& tokens) {
    const std::uint32_t begin = static_cast<std::uint32_t>(tk.size());
    const std::uint32_t srcBase = static_cast<std::uint32_t>(src.size());
    if (tk.empty()) {
        tk.reserve(tokens.size() + tokens.size() / 16 + 16);
        src.reserve(tokens.size());
    }
    std::uint32_t i = srcBase;
    for (Token& t : tokens) {
        Tk k;
        k.kind = static_cast<std::uint8_t>(t.kind);
        k.src = i;
        if (t.kind == TokKind::End) {
            ++i;
            src.push_back(std::move(t));
            continue;
        }
        if (t.kind == TokKind::Ident) {
            k.sym = intern(t.text);
            tk.push_back(k);
        } else if (t.kind == TokKind::Punct) {
            if (t.text == ">>") {
                k.sym = pGt;
                tk.push_back(k);
                k.tight = 1;
                tk.push_back(k);
            } else if (t.text == ">>=") {
                k.sym = pGt;
                tk.push_back(k);
                k.tight = 1;
                k.sym = pGe;
                tk.push_back(k);
            } else {
                k.sym = intern(t.text);
                tk.push_back(k);
            }
        } else {
            tk.push_back(k);
        }
        ++i;
        src.push_back(std::move(t));
    }
    return TokRange{begin, static_cast<std::uint32_t>(tk.size())};
}

std::string Program::Impl::tokenSpelling(std::uint32_t i) const {
    if (i >= tk.size()) return {};
    const Tk& t = tk[i];
    if (t.sym != 0) return symText[t.sym];
    return src[t.src].text;
}

std::string Program::Impl::rangeText(TokRange r) const {
    std::string out;
    const Tk* prev = nullptr;
    for (std::uint32_t i = r.b; i < r.e && i < tk.size(); ++i) {
        const Tk& t = tk[i];
        const std::string s = tokenSpelling(i);
        if (prev) {
            const bool prevWord = prev->kind != static_cast<std::uint8_t>(TokKind::Punct);
            const bool curWord = t.kind != static_cast<std::uint8_t>(TokKind::Punct);
            bool space = false;
            if (prevWord && curWord) space = true;
            else if (prev->sym == pComma) space = true;
            else if ((prev->sym == pGt || prev->sym == pStar || prev->sym == pAmp) && curWord) space = true;
            else if (prevWord && (t.sym == pStar || t.sym == pAmp) && false) space = true;
            if (t.tight) space = false;
            if (space) out.push_back(' ');
        }
        out += s;
        prev = &t;
    }
    return out;
}

Decl* Program::Impl::newDecl(DeclKind kind, Sym name, Decl* parent, std::uint32_t tkIndex) {
    decls.emplace_back();
    Decl* d = &decls.back();
    d->kind = kind;
    d->name = name;
    d->parent = parent;
    d->id = static_cast<std::uint32_t>(decls.size() - 1);
    if (tkIndex < tk.size()) {
        const Token& t = src[tk[tkIndex].src];
        d->file = t.file;
        d->line = t.line;
        d->srcTok = tk[tkIndex].src;
    }
    if (kind == DeclKind::Variable || kind == DeclKind::Field || kind == DeclKind::Enumerator) valueNames.insert(name);
    return d;
}

void Program::Impl::addMember(Decl* scope, Decl* d, bool registerName) {
    scope->members.push_back(d);
    if (registerName && d->name != 0) scope->byName[d->name].push_back(d);
}

std::string Program::Impl::qualifiedName(const Decl* d) const {
    std::string out;
    std::vector<const Decl*> chain;
    for (const Decl* c = d; c && c != global; c = c->parent) chain.push_back(c);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!out.empty()) out += "::";
        out += (*it)->name != 0 ? symText[(*it)->name] : std::string("<anonymous>");
    }
    return out;
}

void Program::Impl::diag(Diag::Severity sev, std::string msg, std::uint32_t tkIndex) {
    std::uint32_t file = 0, line = 0;
    if (tkIndex < tk.size()) {
        const Token& t = src[tk[tkIndex].src];
        file = t.file;
        line = t.line;
    }
    diagAt(sev, std::move(msg), file, line);
}

void Program::Impl::diagAt(Diag::Severity sev, std::string msg, std::uint32_t file, std::uint32_t line) {
    if (diagList.size() > 20000) return;
    Diag d;
    d.severity = sev;
    d.message = std::move(msg);
    if (file < files.size()) d.file = files[file];
    d.line = line;
    diagList.push_back(std::move(d));
}

std::uint32_t Program::Impl::skipBalanced(std::uint32_t p, std::uint32_t end) const {
    int depth = 0;
    std::uint32_t i = p;
    while (i < end) {
        const Tk& t = tk[i];
        if (t.kind == static_cast<std::uint8_t>(TokKind::Punct)) {
            switch (t.sym) {
            case pLParen: case pLBracket: case pLBrace:
                ++depth;
                break;
            case pRParen: case pRBracket: case pRBrace:
                if (--depth <= 0) return i + 1;
                break;
            default:
                break;
            }
        }
        ++i;
    }
    return end;
}

bool Program::Impl::angleOpens(std::uint32_t p) const {
    if (p == 0 || p >= tk.size()) return false;
    const Tk& prev = tk[p - 1];
    if (prev.kind != static_cast<std::uint8_t>(TokKind::Ident)) return false;
    if (prev.sym < kFirstDynamicSym && prev.sym != kTemplate) {
        // keywords never name a template ("sizeof <", "operator <")
        return false;
    }
    if (templateNames.count(prev.sym)) return true;
    return valueNames.count(prev.sym) == 0;
}

std::uint32_t Program::Impl::skipAngle(std::uint32_t p, std::uint32_t end) const {
    int depth = 0;
    std::uint32_t i = p;
    while (i < end) {
        const Tk& t = tk[i];
        if (t.kind == static_cast<std::uint8_t>(TokKind::Punct)) {
            switch (t.sym) {
            case pLt:
                if (i == p || angleOpens(i)) ++depth;
                break;
            case pGt:
                if (--depth == 0) return i + 1;
                break;
            case pLParen: case pLBracket: case pLBrace:
                i = skipBalanced(i, end);
                continue;
            case pSemi: case pRBrace: case pRParen: case pRBracket:
                return kNpos;
            default:
                break;
            }
        }
        ++i;
    }
    return kNpos;
}

// ---- public API ---------------------------------------------------------------------------------------------

Program::Program() = default;
Program::~Program() = default;

std::unique_ptr<Program> Program::parse(std::vector<Token> tokens, std::vector<std::string> files, Diags& diags,
                                        ProgramOptions options) {
    std::unique_ptr<Program> prog(new Program());
    prog->impl_ = std::make_unique<Impl>(std::move(options));
    Impl& I = *prog->impl_;
    I.files = std::move(files);
    TokRange r = I.appendTokens(std::move(tokens));
    parseDeclarations(I, r.b, r.e);
    I.declCount = I.decls.size();
    if (I.opts.checkStaticAsserts) I.checkStaticAsserts();
    diags.insert(diags.end(), I.diagList.begin(), I.diagList.end());
    return prog;
}

const ProgramOptions& Program::options() const { return impl_->opts; }
const std::vector<std::string>& Program::files() const { return impl_->files; }
std::string Program::fileName(std::uint32_t file) const {
    return file < impl_->files.size() ? impl_->files[file] : std::string();
}
const Diags& Program::diags() const { return impl_->diagList; }
std::size_t Program::declarationCount() const { return impl_->declCount; }

std::string Program::tokenText(std::uint32_t begin, std::uint32_t end) const {
    return impl_->rangeText(TokRange{begin, end});
}

bool Program::Impl::scopeFromName(std::string_view name, Scope& out, std::string& err) {
    out = Scope{global, nullptr};
    if (name.empty()) return true;
    std::vector<Token> toks = lex(name);
    TokRange r = appendTokens(std::move(toks));
    try {
        Sema s(*this, Scope{global, nullptr}, r);
        Entity e = s.parseName(Role::Qualifier, false);
        s.expectEnd();
        if (e.kind == Entity::Kind::Decl && e.decl->kind == DeclKind::Namespace) {
            out = Scope{e.decl, nullptr};
            return true;
        }
        if (e.kind == Entity::Kind::Type) {
            const TypeInfo& ti = types[e.type];
            if (ti.decl) {
                out = Scope{ti.decl, ti.env};
                return true;
            }
        }
        err = "scope '" + std::string(name) + "' is not a namespace or class";
    } catch (const SemaError& ex) {
        err = ex.what();
    }
    return false;
}

TypeId Program::lookupType(std::string_view qualifiedName, std::string_view scope) {
    Impl& I = *impl_;
    Scope sc;
    std::string err;
    if (!I.scopeFromName(scope, sc, err)) {
        I.diagAt(Diag::Severity::Warning, "lookupType: " + err, 0, 0);
        return kNoType;
    }
    TokRange r = I.appendTokens(lex(qualifiedName));
    try {
        Sema s(I, sc, r);
        TypeId t = s.parseTypeId();
        s.expectEnd();
        return t;
    } catch (const SemaError& ex) {
        I.diagAt(Diag::Severity::Warning, "lookupType('" + std::string(qualifiedName) + "'): " + ex.what(), 0, 0);
        return kNoType;
    }
}

const TypeLayout& Program::layoutOf(TypeId id) { return impl_->layoutOf(id); }

std::string Program::typeName(TypeId id) {
    if (id >= impl_->types.size()) return {};
    return impl_->types[id].layout.name;
}

std::vector<TypeId> Program::allTypes() const {
    std::vector<TypeId> v(impl_->types.size());
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<TypeId>(i);
    return v;
}

std::size_t Program::typeCount() const { return impl_->types.size(); }

std::optional<ConstValue> Program::evalConstant(const std::vector<Token>& tokens, std::string_view scope,
                                                std::string* error) {
    Impl& I = *impl_;
    Scope sc;
    std::string err;
    if (!I.scopeFromName(scope, sc, err)) {
        if (error) *error = err;
        I.diagAt(Diag::Severity::Warning, "evalConstant: " + err, 0, 0);
        return std::nullopt;
    }
    std::vector<Token> copy = tokens;
    TokRange r = I.appendTokens(std::move(copy));
    try {
        Sema s(I, sc, r);
        ConstValue v = s.parseExpr();
        s.expectEnd();
        return v;
    } catch (const SemaError& ex) {
        if (error) *error = ex.what();
        I.diagAt(Diag::Severity::Warning, std::string("evalConstant: ") + ex.what(), 0, 0);
        return std::nullopt;
    }
}

std::optional<ConstValue> Program::evalConstant(std::string_view expression, std::string_view scope,
                                                std::string* error) {
    return evalConstant(lex(expression), scope, error);
}

namespace {
VariableInfo makeVariableInfo(Program::Impl& I, const Decl* d, const Env* env) {
    VariableInfo v;
    v.id = d->id;
    v.name = I.text(d->name);
    v.qualifiedName = I.qualifiedName(d);
    v.isConstexpr = d->isConstexpr;
    v.isStatic = d->isStatic;
    v.isExtern = d->isExtern;
    v.hasInitializer = d->hasInit;
    v.initBegin = d->init.b;
    v.initEnd = d->init.e;
    v.loc = SourceLoc{d->file, d->line};
    try {
        v.type = I.typeOfDecl(d, env);
    } catch (const SemaError& ex) {
        I.diagAt(Diag::Severity::Note, "variable '" + v.qualifiedName + "': " + ex.what(), d->file, d->line);
    }
    return v;
}
} // namespace

std::optional<VariableInfo> Program::lookupVariable(std::string_view qualifiedName, std::string_view scope) {
    Impl& I = *impl_;
    Scope sc;
    std::string err;
    if (!I.scopeFromName(scope, sc, err)) return std::nullopt;
    TokRange r = I.appendTokens(lex(qualifiedName));
    try {
        Sema s(I, sc, r);
        Entity e = s.parseName(Role::Value, false);
        if (e.kind == Entity::Kind::Decl && e.decl->kind == DeclKind::Variable) return makeVariableInfo(I, e.decl, e.env);
    } catch (const SemaError& ex) {
        I.diagAt(Diag::Severity::Note, std::string("lookupVariable: ") + ex.what(), 0, 0);
    }
    return std::nullopt;
}

std::vector<VariableInfo> Program::variables(std::string_view scope) {
    Impl& I = *impl_;
    std::vector<VariableInfo> out;
    Scope sc;
    std::string err;
    if (!I.scopeFromName(scope, sc, err)) return out;
    for (const Decl* d : sc.decl->members)
        if (d->kind == DeclKind::Variable) out.push_back(makeVariableInfo(I, d, sc.env));
    return out;
}

std::optional<InitValue> Program::evalInitializer(const VariableInfo& variable) {
    Impl& I = *impl_;
    if (variable.id >= I.decls.size()) return std::nullopt;
    const Decl* d = &I.decls[variable.id];
    if (d->kind != DeclKind::Variable && d->kind != DeclKind::Field) return std::nullopt;
    if (!d->hasInit) return std::nullopt;
    try {
        // Variables in a template instance are not reachable through this API: environment is null.
        TypeId t = I.typeOfDecl(d, nullptr);
        TokRange r = d->init;
        Sema s(I, Scope{d->parent, nullptr}, r);
        InitValue v = s.parseInit(t, false);
        return v;
    } catch (const SemaError& ex) {
        I.diagAt(Diag::Severity::Warning, std::string("evalInitializer: ") + ex.what(), d->file, d->line);
        return std::nullopt;
    }
}

std::vector<DeclInfo> Program::declarations() const {
    std::vector<DeclInfo> out;
    Impl& I = *impl_;
    for (const Decl& d : I.decls) {
        if (d.name == 0 || &d == I.global) continue;
        DeclInfo info;
        switch (d.kind) {
        case DeclKind::Namespace: info.kind = DeclInfo::Kind::Namespace; break;
        case DeclKind::Record: info.kind = DeclInfo::Kind::Record; break;
        case DeclKind::Enum: info.kind = DeclInfo::Kind::Enum; break;
        case DeclKind::Typedef: info.kind = DeclInfo::Kind::Typedef; break;
        case DeclKind::Variable: info.kind = DeclInfo::Kind::Variable; break;
        default: continue;
        }
        info.qualifiedName = I.qualifiedName(&d);
        info.isTemplate = d.isTemplate;
        info.isDefined = d.defined;
        info.loc = SourceLoc{d.file, d.line};
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace qstate::cpp

namespace qstate::cpp {

std::vector<PackRegion> packRegionsFromPragmas(const std::vector<PragmaRecord>& pragmas, std::uint32_t endToken) {
    std::vector<PackRegion> regions;
    std::vector<std::uint32_t> stack;
    std::uint32_t current = 0;
    std::uint32_t regionBegin = 0;
    auto close = [&](std::uint32_t at) {
        if (current != 0 && at > regionBegin) regions.push_back(PackRegion{regionBegin, at, current});
        regionBegin = at;
    };
    for (const PragmaRecord& pr : pragmas) {
        std::vector<Token> toks = lexFragment(pr.text);
        if (toks.size() < 3 || !toks[0].isIdent("pack") || !toks[1].isPunct("(")) continue;
        std::vector<std::string> items;
        std::string curItem;
        bool haveItem = false;
        for (std::size_t i = 2; i < toks.size(); ++i) {
            if (toks[i].isPunct(")")) break;
            if (toks[i].isPunct(",")) {
                items.push_back(curItem);
                curItem.clear();
                haveItem = false;
                continue;
            }
            curItem = toks[i].text;
            haveItem = true;
        }
        if (haveItem) items.push_back(curItem);
        auto toNumber = [](const std::string& t, std::uint32_t& out) {
            if (t.empty() || !std::isdigit(static_cast<unsigned char>(t[0]))) return false;
            out = static_cast<std::uint32_t>(std::stoul(t));
            return true;
        };
        std::uint32_t next = current;
        std::uint32_t n = 0;
        if (items.empty()) {
            next = 0; // pack() resets
        } else if (items[0] == "push") {
            stack.push_back(current);
            if (items.size() > 1 && toNumber(items.back(), n)) next = n;
        } else if (items[0] == "pop") {
            if (!stack.empty()) {
                next = stack.back();
                stack.pop_back();
            } else {
                next = 0;
            }
            if (items.size() > 1 && toNumber(items.back(), n)) next = n;
        } else if (toNumber(items[0], n)) {
            next = n;
        }
        if (next != current) {
            close(static_cast<std::uint32_t>(pr.tokenIndex));
            current = next;
        }
    }
    close(endToken);
    return regions;
}

} // namespace qstate::cpp
