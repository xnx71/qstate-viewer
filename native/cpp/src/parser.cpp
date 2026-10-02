// Tolerant declaration parser: builds the Decl tree (namespaces, records, enums, typedefs, templates, variables,
// static_asserts) from the preprocessed token stream and skips everything else.
#include <algorithm>
#include <climits>

#include "program_impl.h"

namespace qstate::cpp {
namespace {

constexpr std::uint32_t kNpos = UINT32_MAX;

struct TemplCtx {
    bool present = false;
    bool explicitSpec = false; // template<>
    std::vector<TemplateParam> params;
};

struct Flags {
    bool isStatic = false;
    bool isConstexpr = false;
    bool isConst = false;
    bool isTypedef = false;
    bool isExtern = false;
};

struct Declarator {
    Sym name = 0;
    std::uint32_t nameTok = 0;
    bool qualified = false;
    bool isFunction = false;
    bool special = false; // operator / destructor
    std::uint8_t ptr = 0;
    bool ref = false;
    bool funcPtr = false;
    std::vector<TokRange> extents;
    bool unsized = false;
    bool hasBits = false;
    TokRange bits;
    bool hasInit = false;
    TokRange init;
    TokRange params;   // function: inside of the parameter list
    TokRange body;     // function: inside of the body braces
    bool hasBody = false;
    bool ok = true;
};

class Parser {
public:
    Parser(Program::Impl& impl, std::uint32_t begin, std::uint32_t end) : I(impl), p_(begin), end_(end) {}

    void parseTranslationUnit() {
        parseScopeBody(I.global, false);
    }

private:
    Program::Impl& I;
    std::uint32_t p_;
    std::uint32_t end_;
    int depth_ = 0;
    TokRange* pendingAlign_ = nullptr;

    // ---- token access ----------------------------------------------------------------------------------------
    const Tk& tkAt(std::uint32_t i) const {
        static const Tk endTok{};
        return i < end_ ? I.tk[i] : endTok;
    }
    const Tk& cur() const { return tkAt(p_); }
    static bool isPunctTk(const Tk& t, Sym s) { return t.kind == static_cast<std::uint8_t>(TokKind::Punct) && t.sym == s; }
    static bool isIdentTk(const Tk& t) { return t.kind == static_cast<std::uint8_t>(TokKind::Ident); }
    static bool isKwTk(const Tk& t, Sym s) { return isIdentTk(t) && t.sym == s; }
    bool isP(Sym s) const { return isPunctTk(cur(), s); }
    bool isK(Sym s) const { return isKwTk(cur(), s); }
    bool atEnd() const { return p_ >= end_; }
    bool nextIsP(Sym s) const { return isPunctTk(tkAt(p_ + 1), s); }

    void note(Diag::Severity sev, const std::string& msg) { I.diag(sev, msg, std::min(p_, end_ ? end_ - 1 : 0)); }

    static bool isBuiltinKw(Sym s) {
        switch (s) {
        case kUnsigned: case kSigned: case kShort: case kLong: case kInt: case kChar: case kBool: case kVoid:
        case kFloat: case kDouble: case kWchar: case kChar8: case kChar16: case kChar32: case kInt8: case kInt16:
        case kInt32: case kInt64:
            return true;
        default:
            return false;
        }
    }

    std::uint32_t skipBalanced(std::uint32_t p) const { return I.skipBalanced(p, end_); }

    // Like skipBalanced, but gives up at a ';' outside of braces (an unbalanced '(' in malformed input must not
    // swallow the rest of the file). Returns the index of that ';' in this case.
    std::uint32_t skipBalancedSafe(std::uint32_t p) const {
        int depth = 0;
        int braces = 0;
        for (std::uint32_t i = p; i < end_; ++i) {
            const Tk& t = I.tk[i];
            if (t.kind != static_cast<std::uint8_t>(TokKind::Punct)) continue;
            switch (t.sym) {
            case pLParen: case pLBracket: ++depth; break;
            case pLBrace: ++depth; ++braces; break;
            case pRParen: case pRBracket: if (--depth <= 0) return i + 1; break;
            case pRBrace: --braces; if (--depth <= 0) return i + 1; break;
            case pSemi: if (braces == 0) return i; break;
            default: break;
            }
        }
        return end_;
    }

    // At `alignas` or `__declspec` / `__attribute__`: consumes the specifier; returns the alignment expression
    // range for alignas(N) / __declspec(align(N)) and an empty range for everything else.
    TokRange parseAlignSpecifier() {
        const bool isAlignas = isK(kAlignas);
        ++p_;
        if (!isP(pLParen)) return TokRange{};
        const std::uint32_t open = p_;
        p_ = skipBalanced(p_);
        TokRange inner{open + 1, p_ > open + 1 ? p_ - 1 : p_};
        if (isAlignas) return inner;
        if (inner.e >= inner.b + 4 && isIdentTk(I.tk[inner.b]) && I.text(I.tk[inner.b].sym) == "align" &&
            isPunctTk(I.tk[inner.b + 1], pLParen))
            return TokRange{inner.b + 2, inner.e - 1};
        return TokRange{};
    }

    // ---- skipping ----------------------------------------------------------------------------------------------
    // Skips to the end of the current declaration: past ';' at depth 0, or past a '{...}' block (and a following ';').
    void skipDeclEnd() {
        std::uint32_t guard = p_;
        while (!atEnd()) {
            const Tk& t = cur();
            if (t.kind == static_cast<std::uint8_t>(TokKind::Punct)) {
                if (t.sym == pSemi) { ++p_; return; }
                if (t.sym == pLBrace) {
                    p_ = skipBalanced(p_);
                    if (isP(pSemi)) ++p_;
                    return;
                }
                if (t.sym == pLParen || t.sym == pLBracket) { p_ = skipBalancedSafe(p_); continue; }
                if (t.sym == pRBrace) return; // closing brace of the enclosing scope
            }
            ++p_;
        }
        if (p_ == guard) ++p_;
    }

    // Error recovery: skip to the next ';' at depth 0 / past a '{...}' block; always makes progress unless
    // positioned at a closing brace.
    void recoverSkip() {
        if (isP(pRBrace)) return;
        skipDeclEnd();
    }

    // Scans an initializer / expression: stops (without consuming) at ',' ';' or an unbalanced closer.
    std::uint32_t scanExprEnd(std::uint32_t q, bool stopAtGt = false) const {
        while (q < end_) {
            const Tk& t = I.tk[q];
            if (t.kind == static_cast<std::uint8_t>(TokKind::Punct)) {
                switch (t.sym) {
                case pComma: case pSemi: case pRBrace: case pRParen: case pRBracket:
                    return q;
                case pGt:
                    if (stopAtGt) return q;
                    break;
                case pLParen: case pLBracket: case pLBrace:
                    q = skipBalanced(q);
                    continue;
                case pLt:
                    if (I.angleOpens(q)) {
                        std::uint32_t r = I.skipAngle(q, end_);
                        if (r != kNpos) { q = r; continue; }
                    }
                    break;
                default:
                    break;
                }
            }
            ++q;
        }
        return q;
    }

    // q at '(' after a type name: "(*name)(...)" / "(__cdecl* name)(...)" => function pointer declarator.
    bool looksLikeFuncPtr(std::uint32_t q) const {
        std::uint32_t r = q + 1;
        if (r < end_ && isIdentTk(I.tk[r]) && (I.tk[r].sym == kCdecl || I.tk[r].sym == kStdcall)) ++r;
        return r < end_ && (isPunctTk(I.tk[r], pStar) || isPunctTk(I.tk[r], pAmp));
    }

    // Skips a (qualified) name with template arguments starting at q; returns the index after it.
    // Stops at (but does not include) `operator`.
    std::uint32_t skipChain(std::uint32_t q, bool* hadOperator = nullptr) const {
        if (q < end_ && isPunctTk(I.tk[q], pScope)) ++q;
        while (q < end_ && isIdentTk(I.tk[q])) {
            if (I.tk[q].sym == kOperator) {
                if (hadOperator) *hadOperator = true;
                return q;
            }
            ++q;
            if (q < end_ && isPunctTk(I.tk[q], pLt)) {
                std::uint32_t r = I.skipAngle(q, end_);
                if (r == kNpos) return q;
                q = r;
            }
            if (q < end_ && isPunctTk(I.tk[q], pScope)) {
                ++q;
                if (q < end_ && isKwTk(I.tk[q], kTemplate)) ++q;
                if (q < end_ && isPunctTk(I.tk[q], pTilde)) ++q;
                continue;
            }
            break;
        }
        return q;
    }

    // At `operator`: skips the operator-function-id (up to, not including, the parameter list '(').
    void skipOperatorName() {
        ++p_; // operator
        if (isP(pLParen) && nextIsP(pRParen)) { p_ += 2; return; }
        if (isP(pLBracket) && nextIsP(pRBracket)) { p_ += 2; return; }
        if (isK(kNew) || isK(kDelete)) {
            ++p_;
            if (isP(pLBracket) && nextIsP(pRBracket)) p_ += 2;
            return;
        }
        if (cur().kind == static_cast<std::uint8_t>(TokKind::StringLit)) { ++p_; if (isIdentTk(cur())) ++p_; return; }
        if (cur().kind == static_cast<std::uint8_t>(TokKind::Punct) && !isP(pLParen)) {
            ++p_;
            while (cur().tight) ++p_; // split ">>" / ">>="
            return;
        }
        // conversion function: operator Type ( ... )
        while (!atEnd() && !isP(pLParen) && !isP(pSemi) && !isP(pLBrace)) {
            if (isP(pLt)) {
                std::uint32_t r = I.skipAngle(p_, end_);
                if (r == kNpos) { ++p_; continue; }
                p_ = r;
            } else {
                ++p_;
            }
        }
    }

    // ---- scopes ------------------------------------------------------------------------------------------------
    void parseScopeBody(Decl* scope, bool braced) {
        while (!atEnd()) {
            if (isP(pRBrace)) {
                if (braced) return;
                note(Diag::Severity::Warning, "unbalanced '}' at namespace scope");
                ++p_;
                continue;
            }
            std::uint32_t start = p_;
            parseMember(scope);
            if (p_ == start) ++p_; // progress guarantee
        }
    }

    void parseMember(Decl* scope) {
        const Tk& t = cur();
        if (t.kind == static_cast<std::uint8_t>(TokKind::Punct)) {
            if (t.sym == pSemi) { ++p_; return; }
            if (t.sym == pLBracket && nextIsP(pLBracket)) { p_ = skipBalanced(p_); return; }
            parseDeclaration(scope, nullptr);
            return;
        }
        if (t.kind != static_cast<std::uint8_t>(TokKind::Ident)) {
            note(Diag::Severity::Warning, "unexpected literal at declaration level");
            recoverSkip();
            return;
        }
        switch (t.sym) {
        case kNamespace:
            parseNamespace(scope);
            return;
        case kInline:
            if (isKwTk(tkAt(p_ + 1), kNamespace)) { ++p_; parseNamespace(scope, true); return; }
            break;
        case kUsing:
            parseUsing(scope, nullptr);
            return;
        case kTemplate:
            parseTemplate(scope);
            return;
        case kStaticAssert:
            parseStaticAssert(scope);
            return;
        case kPublic: case kPrivate: case kProtected:
            if (nextIsP(pColon)) { p_ += 2; return; }
            break;
        case kExtern:
            if (tkAt(p_ + 1).kind == static_cast<std::uint8_t>(TokKind::StringLit)) {
                p_ += 2;
                if (isP(pLBrace)) {
                    ++p_;
                    parseScopeBody(scope, true);
                    if (isP(pRBrace)) ++p_;
                    return;
                }
                parseMember(scope);
                return;
            }
            break;
        case kFriend:
            skipDeclEnd();
            return;
        default:
            break;
        }
        parseDeclaration(scope, nullptr);
    }

    void parseNamespace(Decl* scope, bool isInline = false) {
        ++p_; // namespace
        while (isK(kAttribute) || isK(kDeclspec)) { ++p_; if (isP(pLParen)) p_ = skipBalanced(p_); }
        if (isP(pLBracket) && nextIsP(pLBracket)) p_ = skipBalanced(p_);
        std::vector<Sym> names;
        std::uint32_t nameTok = p_;
        while (isIdentTk(cur()) && !isK(kInline)) {
            names.push_back(cur().sym);
            ++p_;
            if (isP(pScope)) { ++p_; if (isK(kInline)) ++p_; continue; }
            break;
        }
        if (isP(pAssign)) { skipDeclEnd(); return; } // namespace alias
        if (!isP(pLBrace)) {
            note(Diag::Severity::Warning, "expected '{' after namespace name");
            recoverSkip();
            return;
        }
        ++p_;
        Decl* ns = scope;
        for (Sym n : names) {
            Decl* found = nullptr;
            auto it = ns->byName.find(n);
            if (it != ns->byName.end()) {
                for (Decl* d : it->second)
                    if (d->kind == DeclKind::Namespace) found = d;
            }
            if (!found) {
                found = I.newDecl(DeclKind::Namespace, n, ns, nameTok);
                found->defined = true;
                I.addMember(ns, found, true);
            }
            ns = found;
        }
        if (isInline && ns != scope && std::find(scope->usings.begin(), scope->usings.end(), ns) == scope->usings.end())
            scope->usings.push_back(ns);
        parseScopeBody(ns, true);
        if (isP(pRBrace)) ++p_;
        else note(Diag::Severity::Warning, "missing '}' at end of namespace");
    }

    void parseUsing(Decl* scope, TemplCtx* tc) {
        std::uint32_t usingTok = p_;
        ++p_; // using
        if (isK(kNamespace)) {
            ++p_;
            std::uint32_t b = p_;
            std::uint32_t e = scanExprEnd(p_);
            p_ = e;
            if (isP(pSemi)) ++p_;
            try {
                Sema s(I, Scope{scope, nullptr}, TokRange{b, e});
                Entity ent = s.parseName(Role::Qualifier, true);
                if (ent.kind == Entity::Kind::Decl && ent.decl->kind == DeclKind::Namespace)
                    scope->usings.push_back(const_cast<Decl*>(ent.decl));
                else
                    I.diag(Diag::Severity::Note, "using namespace: namespace not found", usingTok);
            } catch (const SemaError& ex) {
                I.diag(Diag::Severity::Note, std::string("using namespace: ") + ex.what(), usingTok);
            }
            return;
        }
        if (isIdentTk(cur()) && nextIsP(pAssign)) {
            Sym name = cur().sym;
            std::uint32_t nameTok = p_;
            p_ += 2;
            if (isK(kStruct) || isK(kClass) || isK(kUnion) || isK(kEnum)) {
                TypeSpec ts;
                bool typeSeen = false;
                bool done = isK(kEnum) ? parseEnumSpecifier(scope, ts, typeSeen) : parseRecordSpecifier(scope, nullptr, ts, typeSeen);
                if (!done) {
                    Decl* d = I.newDecl(DeclKind::Typedef, name, scope, nameTok);
                    d->type = ts;
                    if (ts.inlineDecl && ts.inlineDecl->name == 0) ts.inlineDecl->name = name;
                    I.addMember(scope, d, true);
                    if (isP(pSemi)) ++p_;
                    return;
                }
                return;
            }
            std::uint32_t b = p_;
            std::uint32_t e = scanExprEnd(p_);
            p_ = e;
            if (isP(pSemi)) ++p_;
            Decl* d = I.newDecl(DeclKind::Typedef, name, scope, nameTok);
            d->type.tokens = TokRange{b, e};
            if (tc && tc->present) {
                d->isTemplate = true;
                d->params = tc->params;
                I.templateNames.insert(name);
            }
            I.addMember(scope, d, true);
            return;
        }
        skipDeclEnd(); // using-declaration
    }

    void parseStaticAssert(Decl* scope) {
        std::uint32_t tok = p_;
        ++p_;
        if (!isP(pLParen)) { recoverSkip(); return; }
        ++p_;
        std::uint32_t b = p_;
        std::uint32_t e = scanExprEnd(p_);
        Decl* d = I.newDecl(DeclKind::StaticAssert, 0, scope, tok);
        d->expr = TokRange{b, e};
        I.addMember(scope, d, false);
        // skip message and the closing parenthesis
        p_ = e;
        while (!atEnd() && !isP(pSemi) && !isP(pRBrace)) {
            if (isP(pLParen) || isP(pLBrace) || isP(pLBracket)) p_ = skipBalanced(p_);
            else ++p_;
        }
        if (isP(pSemi)) ++p_;
    }

    // ---- templates ---------------------------------------------------------------------------------------------
    std::uint32_t scanParamEnd(std::uint32_t q, bool typeContext) const {
        while (q < end_) {
            const Tk& t = I.tk[q];
            if (t.kind == static_cast<std::uint8_t>(TokKind::Punct)) {
                switch (t.sym) {
                case pComma: case pGt: case pSemi: case pRBrace:
                    return q;
                case pAssign:
                    typeContext = false;
                    break;
                case pLParen: case pLBracket: case pLBrace:
                    q = skipBalanced(q);
                    continue;
                case pLt:
                    if (typeContext || I.angleOpens(q)) {
                        std::uint32_t r = I.skipAngle(q, end_);
                        if (r != kNpos) { q = r; continue; }
                    }
                    break;
                default:
                    break;
                }
            }
            ++q;
        }
        return q;
    }

    void parseTemplateParams(std::vector<TemplateParam>& out) {
        ++p_; // '<'
        while (!atEnd() && !isP(pGt)) {
            TemplateParam tp;
            std::uint32_t start = p_;
            if (isK(kTypename) || isK(kClass)) {
                ++p_;
                if (isP(pEllipsis)) { tp.isPack = true; ++p_; }
                if (isIdentTk(cur()) && !isP(pAssign)) { tp.name = cur().sym; ++p_; }
                if (isP(pAssign)) {
                    ++p_;
                    std::uint32_t e = scanParamEnd(p_, true);
                    tp.hasDefault = true;
                    tp.defaultRange = TokRange{p_, e};
                    p_ = e;
                }
            } else if (isK(kTemplate)) {
                ++p_;
                if (isP(pLt)) {
                    std::uint32_t r = I.skipAngle(p_, end_);
                    p_ = r == kNpos ? p_ + 1 : r;
                }
                if (isK(kClass) || isK(kTypename)) ++p_;
                if (isP(pEllipsis)) { tp.isPack = true; ++p_; }
                if (isIdentTk(cur())) { tp.name = cur().sym; ++p_; }
                if (isP(pAssign)) {
                    ++p_;
                    std::uint32_t e = scanParamEnd(p_, true);
                    tp.hasDefault = true;
                    tp.defaultRange = TokRange{p_, e};
                    p_ = e;
                }
            } else {
                tp.isType = false;
                std::uint32_t e = scanParamEnd(p_, true);
                // tokens [p_, e) before '=' : type + name
                std::uint32_t typeEnd = e;
                std::uint32_t q = p_;
                while (q < e && !isPunctTk(I.tk[q], pAssign)) {
                    if (isPunctTk(I.tk[q], pLt)) {
                        std::uint32_t r = I.skipAngle(q, e);
                        q = r == kNpos ? q + 1 : r;
                    } else if (isPunctTk(I.tk[q], pLParen) || isPunctTk(I.tk[q], pLBracket)) {
                        q = I.skipBalanced(q, e);
                    } else {
                        ++q;
                    }
                }
                typeEnd = q;
                std::uint32_t nameIdx = kNpos;
                if (typeEnd > p_ && isIdentTk(I.tk[typeEnd - 1]) && typeEnd - 1 > p_ && !isBuiltinKw(I.tk[typeEnd - 1].sym))
                    nameIdx = typeEnd - 1;
                if (typeEnd - p_ >= 2 && isPunctTk(I.tk[typeEnd - 1], pEllipsis)) {
                    tp.isPack = true;
                    --typeEnd;
                    nameIdx = kNpos;
                    if (typeEnd > p_ && isIdentTk(I.tk[typeEnd - 1]) && typeEnd - 1 > p_ && !isBuiltinKw(I.tk[typeEnd - 1].sym))
                        nameIdx = typeEnd - 1;
                }
                if (nameIdx != kNpos) {
                    tp.name = I.tk[nameIdx].sym;
                    tp.typeRange = TokRange{p_, nameIdx};
                } else {
                    tp.typeRange = TokRange{p_, typeEnd};
                }
                if (q < e && isPunctTk(I.tk[q], pAssign)) {
                    tp.hasDefault = true;
                    tp.defaultRange = TokRange{q + 1, e};
                }
                p_ = e;
            }
            out.push_back(std::move(tp));
            if (isP(pComma)) ++p_;
            else if (!isP(pGt)) {
                note(Diag::Severity::Warning, "malformed template parameter list");
                if (p_ == start) ++p_;
                break;
            }
        }
        if (isP(pGt)) ++p_;
    }

    void parseTemplate(Decl* scope) {
        ++p_; // template
        if (!isP(pLt)) { skipDeclEnd(); return; } // explicit instantiation
        TemplCtx tc;
        tc.present = true;
        parseTemplateParams(tc.params);
        tc.explicitSpec = tc.params.empty();
        while (isK(kTemplate) && nextIsP(pLt)) { // member template of a class template defined out of line
            ++p_;
            std::vector<TemplateParam> junk;
            parseTemplateParams(junk);
            tc.explicitSpec = false;
        }
        if (isK(kUsing)) {
            parseUsing(scope, &tc);
            return;
        }
        parseDeclaration(scope, &tc);
    }

    // ---- declarations ------------------------------------------------------------------------------------------
    Decl* findLocal(Decl* scope, Sym name, DeclKind kind, bool wantTemplate) {
        auto it = scope->byName.find(name);
        if (it == scope->byName.end()) return nullptr;
        for (Decl* d : it->second)
            if (d->kind == kind && d->isTemplate == wantTemplate && !d->isSpecialization) return d;
        return nullptr;
    }

    void mergeParams(Decl* d, const std::vector<TemplateParam>& params, bool definition) {
        if (d->params.empty()) {
            d->params = params;
            return;
        }
        if (d->params.size() == params.size()) {
            for (std::size_t i = 0; i < params.size(); ++i) {
                TemplateParam merged = definition ? params[i] : d->params[i];
                if (!merged.hasDefault) {
                    const TemplateParam& other = definition ? d->params[i] : params[i];
                    if (other.hasDefault) {
                        merged.hasDefault = true;
                        merged.defaultRange = other.defaultRange;
                    }
                }
                d->params[i] = merged;
            }
        }
    }

    Decl* findOrCreatePrimary(Decl* scope, Sym name, RecordKind rk, std::uint32_t tok) {
        Decl* d = findLocal(scope, name, DeclKind::Record, true);
        if (!d) {
            d = I.newDecl(DeclKind::Record, name, scope, tok);
            d->recordKind = rk;
            d->isTemplate = true;
            I.addMember(scope, d, true);
            I.templateNames.insert(name);
        }
        return d;
    }

    // Declares (or finds) the record described by the header just parsed. `definition`: a body follows.
    Decl* declareRecord(Decl* scope, Sym name, RecordKind rk, TemplCtx* tc, bool haveSpec,
                        const std::vector<TokRange>& specArgs, bool definition, std::uint32_t tok) {
        const bool templ = tc && tc->present;
        if (templ && (haveSpec || tc->explicitSpec) && name != 0) {
            Decl* primary = findOrCreatePrimary(scope, name, rk, tok);
            Decl* spec = I.newDecl(DeclKind::Record, name, scope, tok);
            spec->recordKind = rk;
            spec->isTemplate = true;
            spec->isSpecialization = true;
            spec->primary = primary;
            spec->params = tc->params;
            spec->specArgs = specArgs;
            primary->specs.push_back(spec);
            return spec;
        }
        if (templ && name != 0) {
            Decl* d = findLocal(scope, name, DeclKind::Record, true);
            if (d) {
                if (d->defined && definition) {
                    note(Diag::Severity::Note, "redefinition of class template '" + I.text(name) + "' ignored");
                    Decl* dummy = I.newDecl(DeclKind::Record, name, scope, tok);
                    dummy->recordKind = rk;
                    return dummy;
                }
                mergeParams(d, tc->params, definition);
                d->recordKind = rk;
                return d;
            }
            d = I.newDecl(DeclKind::Record, name, scope, tok);
            d->recordKind = rk;
            d->isTemplate = true;
            d->params = tc->params;
            I.addMember(scope, d, true);
            I.templateNames.insert(name);
            return d;
        }
        if (name == 0) {
            Decl* d = I.newDecl(DeclKind::Record, 0, scope, tok);
            d->recordKind = rk;
            d->anonymous = true;
            return d;
        }
        Decl* existing = findLocal(scope, name, DeclKind::Record, false);
        if (existing) {
            if (existing->defined && definition) {
                note(Diag::Severity::Note, "redefinition of '" + I.text(name) + "' ignored");
                Decl* dummy = I.newDecl(DeclKind::Record, name, scope, tok);
                dummy->recordKind = rk;
                return dummy;
            }
            existing->recordKind = rk;
            return existing;
        }
        Decl* d = I.newDecl(DeclKind::Record, name, scope, tok);
        d->recordKind = rk;
        I.addMember(scope, d, true);
        return d;
    }

    // Splits the arguments of a specialization "<...>" into ranges. p_ is at '<'; leaves p_ after '>'.
    bool parseSpecArgs(std::vector<TokRange>& out) {
        std::uint32_t close = I.skipAngle(p_, end_);
        if (close == kNpos) return false;
        std::uint32_t q = p_ + 1;
        const std::uint32_t last = close - 1; // the '>'
        std::uint32_t argBegin = q;
        while (q < last) {
            const Tk& t = I.tk[q];
            if (isPunctTk(t, pComma)) {
                out.push_back(TokRange{argBegin, q});
                argBegin = ++q;
                continue;
            }
            if (isPunctTk(t, pLParen) || isPunctTk(t, pLBracket) || isPunctTk(t, pLBrace)) { q = skipBalanced(q); continue; }
            if (isPunctTk(t, pLt) && (q == p_ + 1 || isIdentTk(I.tk[q - 1]))) {
                std::uint32_t r = I.skipAngle(q, last + 1);
                if (r != kNpos) { q = r; continue; }
            }
            ++q;
        }
        if (argBegin < last) out.push_back(TokRange{argBegin, last});
        p_ = close;
        return true;
    }

    // Returns true when the declaration is complete (forward declaration), false when a type specifier was produced.
    bool parseRecordSpecifier(Decl* scope, TemplCtx* tc, TypeSpec& ts, bool& typeSeen) {
        RecordKind rk = isK(kStruct) ? RecordKind::Struct : isK(kClass) ? RecordKind::Class : RecordKind::Union;
        ++p_;
        TokRange recordAlign;
        while (isK(kDeclspec) || isK(kAttribute) || isK(kAlignas)) {
            if (isK(kAttribute)) {
                ++p_;
                if (isP(pLParen)) p_ = skipBalanced(p_);
                continue;
            }
            TokRange a = parseAlignSpecifier();
            if (!a.empty()) recordAlign = a;
        }
        if (isP(pLBracket) && nextIsP(pLBracket)) p_ = skipBalanced(p_);
        Sym name = 0;
        std::uint32_t nameTok = p_;
        std::vector<TokRange> specArgs;
        bool haveSpec = false;
        if (isIdentTk(cur()) && !isK(kFinal) && !(isK(kAlignas))) {
            // qualified name => elaborated type specifier / out-of-line definition
            if (isPunctTk(tkAt(p_ + 1), pScope)) {
                std::uint32_t q = skipChain(p_);
                ts.tokens = TokRange{p_, q};
                p_ = q;
                if (isP(pLBrace)) { // out-of-line nested class definition: skip
                    p_ = skipBalanced(p_);
                    typeSeen = false;
                    if (isP(pSemi)) ++p_;
                    return true;
                }
                typeSeen = true;
                return false;
            }
            name = cur().sym;
            ++p_;
            if (isP(pLt)) {
                if (!parseSpecArgs(specArgs)) {
                    note(Diag::Severity::Warning, "unbalanced template argument list in class head");
                    recoverSkip();
                    return true;
                }
                haveSpec = true;
            }
        }
        if (isK(kFinal)) ++p_;
        bool hasBases = false;
        std::vector<TokRange> bases;
        if (isP(pColon) && !isPunctTk(tkAt(p_ + 1), pColon)) {
            hasBases = true;
            ++p_;
            parseBases(bases);
        }
        if (isP(pLBrace) || hasBases) {
            if (!isP(pLBrace)) {
                note(Diag::Severity::Warning, "expected '{' after base clause");
                recoverSkip();
                return true;
            }
            Decl* d = declareRecord(scope, name, rk, tc, haveSpec, specArgs, true, nameTok);
            d->defined = true;
            d->alignExpr = recordAlign;
            d->bases = std::move(bases);
            ++p_; // '{'
            if (++depth_ > 200) {
                note(Diag::Severity::Error, "class nesting too deep");
                p_ = end_;
                --depth_;
                return true;
            }
            parseScopeBody(d, true);
            --depth_;
            if (isP(pRBrace)) ++p_;
            else note(Diag::Severity::Warning, "missing '}' at end of class body");
            ts.inlineDecl = d;
            typeSeen = true;
            return false;
        }
        if (isP(pSemi)) {
            if (name != 0) declareRecord(scope, name, rk, tc, haveSpec, specArgs, false, nameTok);
            ++p_;
            return true;
        }
        // elaborated type specifier inside a larger declaration: struct Foo* p;
        if (name != 0) {
            ts.tokens = TokRange{nameTok, nameTok + 1};
            if (haveSpec) ts.tokens.e = p_;
            typeSeen = true;
            return false;
        }
        note(Diag::Severity::Warning, "unrecognised class specifier");
        recoverSkip();
        return true;
    }

    void parseBases(std::vector<TokRange>& out) {
        while (!atEnd() && !isP(pLBrace) && !isP(pSemi)) {
            while (isK(kPublic) || isK(kPrivate) || isK(kProtected) || isK(kVirtual)) ++p_;
            std::uint32_t b = p_;
            std::uint32_t q = skipChain(p_);
            if (q == b) { ++p_; continue; }
            p_ = q;
            if (isP(pEllipsis)) ++p_;
            out.push_back(TokRange{b, q});
            if (isP(pComma)) ++p_;
            else break;
        }
    }

    // Returns true when the declaration is complete.
    bool parseEnumSpecifier(Decl* scope, TypeSpec& ts, bool& typeSeen) {
        ++p_; // enum
        bool scoped = false;
        if (isK(kClass) || isK(kStruct)) { scoped = true; ++p_; }
        while (isK(kDeclspec) || isK(kAttribute)) { ++p_; if (isP(pLParen)) p_ = skipBalanced(p_); }
        Sym name = 0;
        std::uint32_t nameTok = p_;
        if (isIdentTk(cur())) {
            if (isPunctTk(tkAt(p_ + 1), pScope)) { // elaborated qualified
                std::uint32_t q = skipChain(p_);
                ts.tokens = TokRange{p_, q};
                p_ = q;
                typeSeen = true;
                return false;
            }
            name = cur().sym;
            ++p_;
        }
        TokRange underlying;
        bool hasUnderlying = false;
        if (isP(pColon)) {
            ++p_;
            std::uint32_t b = p_;
            while (!atEnd() && !isP(pLBrace) && !isP(pSemi) && !isP(pComma)) {
                if (isP(pLt)) {
                    std::uint32_t r = I.skipAngle(p_, end_);
                    p_ = r == kNpos ? p_ + 1 : r;
                } else {
                    ++p_;
                }
            }
            underlying = TokRange{b, p_};
            hasUnderlying = true;
        }
        if (isP(pLBrace) || isP(pSemi)) {
            Decl* d = nullptr;
            if (name != 0) {
                auto it = scope->byName.find(name);
                if (it != scope->byName.end())
                    for (Decl* c : it->second)
                        if (c->kind == DeclKind::Enum) d = c;
            }
            const bool isDef = isP(pLBrace);
            if (d && d->defined && isDef) {
                note(Diag::Severity::Note, "redefinition of enum '" + I.text(name) + "' ignored");
                d = nullptr;
                name = 0;
            }
            if (!d) {
                d = I.newDecl(DeclKind::Enum, name, scope, nameTok);
                d->anonymous = name == 0;
                if (name != 0) I.addMember(scope, d, true);
            }
            d->scoped = scoped;
            if (hasUnderlying) {
                d->hasUnderlying = true;
                d->underlying = underlying;
            }
            if (!isDef) { ++p_; return true; }
            d->defined = true;
            ++p_; // '{'
            Decl* prev = nullptr;
            while (!atEnd() && !isP(pRBrace)) {
                if (!isIdentTk(cur())) { note(Diag::Severity::Warning, "malformed enumerator list"); ++p_; continue; }
                Decl* e = I.newDecl(DeclKind::Enumerator, cur().sym, d, p_);
                ++p_;
                while (isP(pLBracket) && nextIsP(pLBracket)) p_ = skipBalanced(p_);
                if (isP(pAssign)) {
                    ++p_;
                    std::uint32_t b = p_;
                    std::uint32_t en = scanExprEnd(p_);
                    e->hasValue = true;
                    e->value = TokRange{b, en};
                    p_ = en;
                }
                e->prevEnumerator = prev;
                prev = e;
                d->members.push_back(e);
                d->byName[e->name].push_back(e);
                if (!scoped) {
                    scope->byName[e->name].push_back(e);
                }
                if (isP(pComma)) ++p_;
                else if (!isP(pRBrace)) {
                    note(Diag::Severity::Warning, "expected ',' or '}' in enumerator list");
                    break;
                }
            }
            if (isP(pRBrace)) ++p_;
            ts.inlineDecl = d;
            typeSeen = true;
            return false;
        }
        // elaborated enum type in a declaration
        if (name != 0) {
            ts.tokens = TokRange{nameTok, nameTok + 1};
            typeSeen = true;
            return false;
        }
        recoverSkip();
        return true;
    }

    // Parses one declarator. Leaves p_ after the declarator (and, for functions, after the parameter list and
    // trailing specifiers; the body, when present, is skipped too: `hasBody`).
    TokRange lastBody_;
    bool parseFunctionTail() {
        // returns true when a body was consumed (its range is left in lastBody_)
        for (;;) {
            if (atEnd()) return false;
            const Tk& t = cur();
            if (isIdentTk(t)) {
                if (t.sym == kConst || t.sym == kVolatile || t.sym == kOverride || t.sym == kFinal) { ++p_; continue; }
                if (t.sym == kNoexcept || t.sym == kThrow || t.sym == kAttribute || t.sym == kDeclspec) {
                    ++p_;
                    if (isP(pLParen)) p_ = skipBalanced(p_);
                    continue;
                }
                ++p_; // unknown identifier (macro qualifier)
                continue;
            }
            if (t.kind == static_cast<std::uint8_t>(TokKind::Punct)) {
                if (t.sym == pAmp || t.sym == pAmpAmp) { ++p_; continue; }
                if (t.sym == pArrow) {
                    ++p_;
                    while (!atEnd() && !isP(pLBrace) && !isP(pSemi) && !isP(pAssign)) {
                        if (isP(pLParen) || isP(pLBracket)) p_ = skipBalanced(p_);
                        else ++p_;
                    }
                    continue;
                }
                if (t.sym == pAssign) {
                    ++p_;
                    if (!atEnd() && !isP(pSemi)) ++p_; // 0 / default / delete
                    return false;
                }
                if (t.sym == pColon) { // constructor initializer list
                    ++p_;
                    for (;;) {
                        std::uint32_t q = skipChain(p_);
                        if (q == p_) break;
                        p_ = q;
                        if (isP(pLParen) || isP(pLBrace)) p_ = skipBalanced(p_);
                        if (isP(pEllipsis)) ++p_;
                        if (isP(pComma)) { ++p_; continue; }
                        break;
                    }
                    continue;
                }
                if (t.sym == pLBrace) {
                    const std::uint32_t b = p_;
                    p_ = skipBalanced(p_);
                    lastBody_ = TokRange{b + 1, p_ > b + 1 ? p_ - 1 : p_};
                    return true;
                }
                if (t.sym == pLBracket && nextIsP(pLBracket)) { p_ = skipBalanced(p_); continue; }
            }
            return false;
        }
    }

    void parseDeclarator(Declarator& d, bool& bodyConsumed) {
        bodyConsumed = false;
        for (;;) {
            if (isP(pStar)) { ++d.ptr; ++p_; continue; }
            if (isP(pAmp) || isP(pAmpAmp)) { d.ref = true; ++p_; continue; }
            if (isIdentTk(cur())) {
                Sym s = cur().sym;
                if (s == kConst || s == kVolatile || s == kPtr64 || s == kRestrict || s == kCdecl || s == kStdcall) { ++p_; continue; }
                if ((s == kDeclspec || s == kAttribute)) { ++p_; if (isP(pLParen)) p_ = skipBalanced(p_); continue; }
                // pointer to member: A::*  /  Ident :: *
                std::uint32_t q = skipChain(p_);
                if (q > p_ && isPunctTk(tkAt(q), pScope) && isPunctTk(tkAt(q + 1), pStar)) { p_ = q + 2; ++d.ptr; continue; }
            }
            break;
        }
        if (isP(pLParen)) {
            // function pointer / parenthesized declarator
            std::uint32_t close = skipBalanced(p_);
            std::uint32_t q = p_ + 1;
            bool sawStar = false;
            while (q + 1 < close) {
                const Tk& t = I.tk[q];
                if (isPunctTk(t, pStar) || isPunctTk(t, pAmp)) { sawStar = true; ++q; continue; }
                if (isIdentTk(t) && (isPunctTk(I.tk[q + 1], pStar) || isPunctTk(I.tk[q + 1], pAmp) || isIdentTk(I.tk[q + 1])) && !sawStar) { ++q; continue; }
                break;
            }
            if (sawStar) {
                d.funcPtr = true;
                // optional name and array suffix inside
                std::uint32_t r = q;
                while (r + 1 < close && (isPunctTk(I.tk[r], pStar) || isPunctTk(I.tk[r], pAmp) || isKwTk(I.tk[r], kConst) || isKwTk(I.tk[r], kCdecl) || isKwTk(I.tk[r], kStdcall))) ++r;
                if (r + 1 < close + 0 && isIdentTk(I.tk[r])) {
                    d.name = I.tk[r].sym;
                    d.nameTok = r;
                    ++r;
                }
                while (r < close - 1 && isPunctTk(I.tk[r], pLBracket)) {
                    std::uint32_t rc = skipBalanced(r);
                    if (rc - r > 2) d.extents.push_back(TokRange{r + 1, rc - 1});
                    r = rc;
                }
                p_ = close;
                if (isP(pLParen)) p_ = skipBalanced(p_); // parameter list
                else d.funcPtr = true;
                // trailing qualifiers
                while (isK(kConst) || isK(kNoexcept) || isK(kVolatile)) { ++p_; if (isP(pLParen)) p_ = skipBalanced(p_); }
                d.isFunction = false;
                return;
            }
            // plain parenthesized declarator: (name)
            p_ = close;
            d.ok = false;
            return;
        }
        if (isK(kOperator)) {
            d.special = true;
            skipOperatorName();
        } else if (isP(pTilde)) {
            d.special = true;
            ++p_;
            if (isIdentTk(cur())) ++p_;
        } else if (isIdentTk(cur()) || isP(pScope)) {
            std::uint32_t start = p_;
            bool hadOp = false;
            std::uint32_t q = skipChain(p_, &hadOp);
            // find last Ident at template depth 0 before q: approximate by scanning back over '>' groups
            std::uint32_t scan = start;
            std::uint32_t lastIdent = kNpos;
            bool sawScope = false;
            while (scan < q) {
                if (isIdentTk(I.tk[scan])) { lastIdent = scan; ++scan; if (scan < q && isPunctTk(I.tk[scan], pLt)) { std::uint32_t r = I.skipAngle(scan, q); scan = r == kNpos ? q : r; } continue; }
                if (isPunctTk(I.tk[scan], pScope)) sawScope = true;
                ++scan;
            }
            p_ = q;
            if (hadOp) {
                d.special = true;
                sawScope = true;
                skipOperatorName();
            } else if (lastIdent != kNpos) {
                d.name = I.tk[lastIdent].sym;
                d.nameTok = lastIdent;
            }
            d.qualified = sawScope;
        }
        // suffixes
        for (;;) {
            if (atEnd()) return;
            if (isP(pLBracket)) {
                if (nextIsP(pLBracket)) { p_ = skipBalanced(p_); continue; } // attribute
                std::uint32_t close = skipBalanced(p_);
                if (close - p_ <= 2) d.unsized = true;
                else d.extents.push_back(TokRange{p_ + 1, close - 1});
                p_ = close;
                continue;
            }
            if (isP(pLParen)) {
                d.isFunction = true;
                const std::uint32_t open = p_;
                p_ = skipBalanced(p_);
                d.params = TokRange{open + 1, p_ > open + 1 ? p_ - 1 : p_};
                lastBody_ = TokRange{};
                bodyConsumed = parseFunctionTail();
                d.hasBody = bodyConsumed;
                d.body = lastBody_;
                return;
            }
            if (isP(pColon)) {
                ++p_;
                std::uint32_t e = scanExprEnd(p_);
                d.hasBits = true;
                d.bits = TokRange{p_, e};
                p_ = e;
                continue;
            }
            if (isP(pAssign)) {
                ++p_;
                std::uint32_t e = scanExprEnd(p_);
                d.hasInit = true;
                d.init = TokRange{p_, e};
                p_ = e;
                continue;
            }
            if (isP(pLBrace)) {
                std::uint32_t close = skipBalanced(p_);
                d.hasInit = true;
                d.init = TokRange{p_, close};
                p_ = close;
                continue;
            }
            return;
        }
    }

    void parseDeclaration(Decl* scope, TemplCtx* tc) {
        Flags f;
        TypeSpec ts;
        bool typeSeen = false;
        TokRange typeRange;
        TokRange pendingAlign;
        struct AlignScope {
            TokRange*& slot;
            TokRange* saved;
            AlignScope(TokRange*& s, TokRange* n) : slot(s), saved(s) { slot = n; }
            ~AlignScope() { slot = saved; }
        } alignScope(pendingAlign_, &pendingAlign);
        const std::uint32_t startTok = p_;
        const bool inRecord = scope->kind == DeclKind::Record;

        for (;;) {
            if (atEnd()) break;
            const Tk& t = cur();
            if (t.kind == static_cast<std::uint8_t>(TokKind::Punct)) {
                if (t.sym == pLBracket && nextIsP(pLBracket)) { p_ = skipBalanced(p_); continue; }
                break;
            }
            if (t.kind != static_cast<std::uint8_t>(TokKind::Ident)) break;
            bool handled = true;
            switch (t.sym) {
            case kStatic: f.isStatic = true; ++p_; break;
            case kConstexpr: f.isConstexpr = true; f.isConst = true; ++p_; break;
            case kConst: f.isConst = true; ++p_; break;
            case kTypedef: f.isTypedef = true; ++p_; break;
            case kExtern:
                f.isExtern = true;
                ++p_;
                if (cur().kind == static_cast<std::uint8_t>(TokKind::StringLit)) ++p_;
                break;
            case kFriend:
                skipDeclEnd();
                return;
            case kInline: case kVolatile: case kVirtual: case kExplicit: case kMutable: case kRegister:
            case kThreadLocal: case kForceinline: case kInlineNs: case kConstinit: case kConsteval: case kCdecl:
            case kStdcall: case kRestrict: case kPtr64: case kTypename:
                ++p_;
                break;
            case kDeclspec: case kAlignas: {
                TokRange a = parseAlignSpecifier();
                if (!a.empty()) pendingAlign = a;
                break;
            }
            case kAttribute: case kAsm:
                ++p_;
                if (isP(pLParen)) p_ = skipBalanced(p_);
                break;
            case kStruct: case kClass: case kUnion:
                if (parseRecordSpecifier(scope, tc, ts, typeSeen)) return;
                break;
            case kEnum:
                if (parseEnumSpecifier(scope, ts, typeSeen)) return;
                break;
            case kDecltype:
                ++p_;
                if (isP(pLParen)) p_ = skipBalanced(p_);
                ts.isAuto = true;
                typeSeen = true;
                break;
            case kAuto:
                ts.isAuto = true;
                typeSeen = true;
                ++p_;
                break;
            case kOperator:
                handled = false;
                break;
            default:
                if (isBuiltinKw(t.sym)) {
                    std::uint32_t b = p_;
                    while (isIdentTk(cur()) && (isBuiltinKw(cur().sym) || cur().sym == kConst || cur().sym == kVolatile)) ++p_;
                    if (!typeSeen) typeRange = TokRange{b, p_};
                    typeSeen = true;
                    break;
                }
                {
                    std::uint32_t q = skipChain(p_);
                    if (q == p_) { handled = false; break; }
                    const Tk& next = tkAt(q);
                    const bool nextIsDeclTerm =
                        next.kind == static_cast<std::uint8_t>(TokKind::Punct) &&
                        (next.sym == pSemi || next.sym == pComma || next.sym == pAssign || next.sym == pLBracket ||
                         next.sym == pColon || next.sym == pLBrace || next.sym == pRParen);
                    if (!typeSeen) {
                        if (isPunctTk(next, pLParen) && looksLikeFuncPtr(q)) {
                            typeRange = TokRange{p_, q};
                            typeSeen = true;
                            p_ = q;
                            break;
                        }
                        if (isPunctTk(next, pLParen) || nextIsDeclTerm) { handled = false; break; }
                        typeRange = TokRange{p_, q};
                        typeSeen = true;
                        p_ = q;
                    } else {
                        if (isPunctTk(next, pLParen) || nextIsDeclTerm) { handled = false; break; }
                        p_ = q; // attribute-like macro / second word: ignore
                    }
                }
                break;
            }
            if (!handled) break;
        }

        if (typeSeen && ts.tokens.empty() && !ts.inlineDecl && !ts.isAuto) ts.tokens = typeRange;
        else if (typeSeen && ts.tokens.empty() && !ts.inlineDecl) ts.tokens = TokRange{};

        if (atEnd()) return;
        if (isP(pSemi)) {
            // type-only declaration: anonymous struct / union member
            if (ts.inlineDecl && ts.inlineDecl->anonymous && inRecord && !f.isTypedef && ts.inlineDecl->kind == DeclKind::Record) {
                Decl* fd = I.newDecl(DeclKind::Field, 0, scope, startTok);
                fd->type = ts;
                fd->anonymous = true;
                I.addMember(scope, fd, false);
            }
            ++p_;
            return;
        }
        if (!typeSeen && !isIdentTk(cur()) && !isP(pStar) && !isP(pAmp) && !isP(pTilde) && !isP(pLParen) && !isP(pScope)) {
            note(Diag::Severity::Warning, "unrecognised declaration");
            recoverSkip();
            return;
        }

        // declarators
        bool first = true;
        for (;;) {
            Declarator d;
            bool body = false;
            const std::uint32_t before = p_;
            parseDeclarator(d, body);
            if (!d.ok || p_ == before) {
                if (first) note(Diag::Severity::Note, "unrecognised declarator");
                recoverSkip();
                return;
            }
            first = false;
            if (d.isFunction) {
                if (f.isTypedef && d.name != 0 && !d.qualified) {
                    // typedef R F(args): function type; treat as pointer-sized alias
                    Decl* td = I.newDecl(DeclKind::Typedef, d.name, scope, d.nameTok);
                    td->type = ts;
                    td->type.isFuncPtr = true;
                    td->isFunctionTypedef = true;
                    I.addMember(scope, td, true);
                }
                if (body && f.isConstexpr && d.name != 0 && !d.qualified && !d.special && !f.isTypedef) createFunction(scope, f, ts, d, tc);
                if (body) return;
            } else if (d.name != 0 && !d.qualified && !d.special) {
                if (!(tc && tc->present && !tc->params.empty()) || f.isTypedef) createDecl(scope, f, ts, d, tc);
            } else if (d.name == 0 && !d.special && d.hasBits && inRecord && !f.isTypedef) {
                // unnamed bit-field (padding)
                createDecl(scope, f, ts, d, tc);
            }
            if (isP(pComma)) { ++p_; continue; }
            if (isP(pSemi)) { ++p_; return; }
            if (atEnd() || isP(pRBrace)) return;
            note(Diag::Severity::Warning, "expected ';' after declaration");
            recoverSkip();
            return;
        }
    }

    // Records a constexpr function with a body (evaluated on demand by the constant evaluator).
    void createFunction(Decl* scope, const Flags& f, const TypeSpec& retSpec, const Declarator& d, TemplCtx* tc) {
        Decl* fn = I.newDecl(DeclKind::Function, d.name, scope, d.nameTok);
        fn->type = retSpec;
        fn->type.ptrDepth = d.ptr;
        fn->type.isRef = d.ref;
        fn->isStatic = f.isStatic;
        fn->isConstexpr = true;
        fn->body = d.body;
        fn->defined = true;
        if (tc && tc->present && !tc->params.empty()) {
            fn->isTemplate = true;
            fn->params = tc->params;
            I.templateNames.insert(d.name);
        }
        // parameters: split at top-level commas
        std::uint32_t q = d.params.b;
        const std::uint32_t e = d.params.e;
        while (q < e) {
            std::uint32_t argEnd = q;
            while (argEnd < e && !isPunctTk(I.tk[argEnd], pComma)) {
                if (isPunctTk(I.tk[argEnd], pLParen) || isPunctTk(I.tk[argEnd], pLBracket) || isPunctTk(I.tk[argEnd], pLBrace)) argEnd = I.skipBalanced(argEnd, e);
                else if (isPunctTk(I.tk[argEnd], pLt) && I.angleOpens(argEnd)) {
                    std::uint32_t r = I.skipAngle(argEnd, e);
                    argEnd = r == kNpos ? argEnd + 1 : r;
                } else ++argEnd;
            }
            std::uint32_t declEnd = q;
            while (declEnd < argEnd && !isPunctTk(I.tk[declEnd], pAssign)) {
                if (isPunctTk(I.tk[declEnd], pLt) && I.angleOpens(declEnd)) {
                    std::uint32_t r = I.skipAngle(declEnd, argEnd);
                    declEnd = r == kNpos ? declEnd + 1 : r;
                } else ++declEnd;
            }
            FuncParam fp;
            std::uint32_t typeEnd = declEnd;
            if (typeEnd > q + 1 && isIdentTk(I.tk[typeEnd - 1]) && !isBuiltinKw(I.tk[typeEnd - 1].sym) && I.tk[typeEnd - 1].sym != kConst) {
                fp.name = I.tk[typeEnd - 1].sym;
                --typeEnd;
            }
            while (typeEnd > q && (isPunctTk(I.tk[typeEnd - 1], pStar) || isPunctTk(I.tk[typeEnd - 1], pAmp) || isPunctTk(I.tk[typeEnd - 1], pAmpAmp))) {
                if (isPunctTk(I.tk[typeEnd - 1], pStar)) ++fp.ptrDepth;
                else fp.isRef = true;
                --typeEnd;
            }
            fp.type = TokRange{q, typeEnd};
            if (!(typeEnd == q)) fn->fparams.push_back(fp);
            q = argEnd < e ? argEnd + 1 : e;
        }
        I.addMember(scope, fn, true);
    }

    void createDecl(Decl* scope, const Flags& f, const TypeSpec& baseSpec, const Declarator& d, TemplCtx* tc) {
        const bool inRecord = scope->kind == DeclKind::Record;
        DeclKind kind;
        if (f.isTypedef) kind = DeclKind::Typedef;
        else if (inRecord && !f.isStatic) kind = DeclKind::Field;
        else kind = DeclKind::Variable;
        Decl* decl = I.newDecl(kind, d.name, scope, d.nameTok ? d.nameTok : p_ - 1);
        if (pendingAlign_) decl->alignExpr = *pendingAlign_;
        decl->type = baseSpec;
        decl->type.ptrDepth = static_cast<std::uint8_t>(std::min<int>(d.ptr, 255));
        decl->type.isRef = d.ref;
        decl->type.isFuncPtr = d.funcPtr || baseSpec.isFuncPtr;
        decl->type.extents = d.extents;
        decl->type.unsized = d.unsized;
        decl->isStatic = f.isStatic;
        decl->isConstexpr = f.isConstexpr;
        decl->isConst = f.isConst;
        decl->isExtern = f.isExtern;
        if (d.hasBits) {
            decl->isBitField = true;
            decl->bitWidth = d.bits;
        }
        if (d.hasInit) {
            decl->hasInit = true;
            decl->init = d.init;
        }
        if (kind == DeclKind::Typedef && baseSpec.inlineDecl && baseSpec.inlineDecl->kind == DeclKind::Record &&
            baseSpec.inlineDecl->name == 0 && d.ptr == 0 && d.extents.empty()) {
            baseSpec.inlineDecl->name = d.name; // typedef name for linkage; not registered by name
        }
        (void)tc;
        I.addMember(scope, decl, true);
    }
};

} // namespace

void parseDeclarations(Program::Impl& impl, std::uint32_t begin, std::uint32_t end) {
    Parser parser(impl, begin, end);
    parser.parseTranslationUnit();
}

} // namespace qstate::cpp
