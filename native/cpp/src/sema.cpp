// Interpretation of token ranges: type-ids, template arguments, integral constant expressions, initializers.
#include <algorithm>
#include <cctype>
#include <climits>

#include "program_impl.h"

namespace qstate::cpp {
namespace {

constexpr std::uint8_t kMaskType = 1;
constexpr std::uint8_t kMaskNamespace = 2;
constexpr std::uint8_t kMaskValue = 4;

const Tk kEndTk{};

struct SemaDepth {
    int& d;
    explicit SemaDepth(int& x) : d(x) {
        if (++d > 150) {
            --d;
            throw SemaError("expression nesting too deep");
        }
    }
    ~SemaDepth() { --d; }
};

enum Op {
    OpLOr, OpLAnd, OpOr, OpXor, OpAnd, OpEq, OpNe, OpLt, OpLe, OpGt, OpGe, OpShl, OpShr, OpAdd, OpSub, OpMul, OpDiv, OpMod
};

struct NoEvalGuard {
    int& n;
    bool active;
    NoEvalGuard(int& counter, bool a) : n(counter), active(a) { if (active) ++n; }
    ~NoEvalGuard() { if (active) --n; }
};

struct NoGtGuard {
    bool& flag;
    bool saved;
    NoGtGuard(bool& f, bool v) : flag(f), saved(f) { flag = v; }
    ~NoGtGuard() { flag = saved; }
};

ConstValue promote(ConstValue v) {
    if (v.bits < 32) {
        v.bits = 32;
        v.isSigned = true;
    }
    return v;
}

ConstValue withType(std::int64_t v, std::uint8_t bits, bool isSigned) { return Program::Impl::normalize(v, bits, isSigned); }

void commonType(ConstValue a, ConstValue b, std::uint8_t& bits, bool& isSigned) {
    a = promote(a);
    b = promote(b);
    if (a.bits == 64 && b.bits == 64) {
        bits = 64;
        isSigned = a.isSigned && b.isSigned;
    } else if (a.bits == 64) {
        bits = 64;
        isSigned = a.isSigned;
    } else if (b.bits == 64) {
        bits = 64;
        isSigned = b.isSigned;
    } else {
        bits = 32;
        isSigned = a.isSigned && b.isSigned;
    }
}

ConstValue boolValue(bool b) { return withType(b ? 1 : 0, 1, false); }

} // namespace

const Tk& Sema::cur() const { return p_ < end_ ? I.tk[p_] : kEndTk; }
const Tk& Sema::at(std::uint32_t i) const { return i < end_ ? I.tk[i] : kEndTk; }
bool Sema::isIdent() const { return p_ < end_ && cur().kind == static_cast<std::uint8_t>(TokKind::Ident); }

void Sema::fail(const std::string& msg) const { throw SemaError(msg); }

void Sema::expectP(Sym s, const char* what) {
    if (!isP(s)) fail(std::string("expected '") + what + "'");
    ++p_;
}

void Sema::expectEnd() {
    if (!atEnd()) fail("unexpected token '" + I.tokenSpelling(p_) + "'");
}

bool Sema::isBuiltinTypeKeyword(Sym s) const {
    switch (s) {
    case kUnsigned: case kSigned: case kShort: case kLong: case kInt: case kChar: case kBool: case kVoid: case kFloat:
    case kDouble: case kWchar: case kChar8: case kChar16: case kChar32: case kInt8: case kInt16: case kInt32: case kInt64:
        return true;
    default:
        return false;
    }
}

// ---- types ---------------------------------------------------------------------------------------------------

TypeId Sema::parseBuiltin() {
    int nUnsigned = 0, nSigned = 0, nShort = 0, nLong = 0, nInt = 0, nChar = 0;
    bool isBool = false, isVoid = false, isFloat = false, isDouble = false, isWchar = false;
    bool isC8 = false, isC16 = false, isC32 = false;
    int msBits = 0;
    bool any = false;
    while (isIdent()) {
        Sym s = cur().sym;
        if (s == kConst || s == kVolatile) { ++p_; continue; }
        if (!isBuiltinTypeKeyword(s)) break;
        any = true;
        switch (s) {
        case kUnsigned: ++nUnsigned; break;
        case kSigned: ++nSigned; break;
        case kShort: ++nShort; break;
        case kLong: ++nLong; break;
        case kInt: ++nInt; break;
        case kChar: ++nChar; break;
        case kBool: isBool = true; break;
        case kVoid: isVoid = true; break;
        case kFloat: isFloat = true; break;
        case kDouble: isDouble = true; break;
        case kWchar: isWchar = true; break;
        case kChar8: isC8 = true; break;
        case kChar16: isC16 = true; break;
        case kChar32: isC32 = true; break;
        case kInt8: msBits = 8; break;
        case kInt16: msBits = 16; break;
        case kInt32: msBits = 32; break;
        case kInt64: msBits = 64; break;
        default: break;
        }
        ++p_;
    }
    if (!any) fail("expected a type");
    const bool u = nUnsigned > 0;
    if (isVoid) return I.tVoid;
    if (isBool) return I.tBool;
    if (isFloat) return I.tFloat;
    if (isDouble) return nLong ? I.tLongDouble : I.tDouble;
    if (isWchar) return I.tWchar;
    if (isC8) return I.tChar8;
    if (isC16) return I.tChar16;
    if (isC32) return I.tChar32;
    if (msBits == 8) return u ? I.tUChar : I.tSChar;
    if (msBits == 16) return u ? I.tUShort : I.tShort;
    if (msBits == 32) return u ? I.tUInt : I.tInt;
    if (msBits == 64) return u ? I.tULongLong : I.tLongLong;
    if (nChar) return u ? I.tUChar : (nSigned ? I.tSChar : I.tChar);
    if (nShort) return u ? I.tUShort : I.tShort;
    if (nLong >= 2) return u ? I.tULongLong : I.tLongLong;
    if (nLong == 1) return u ? I.tULong : I.tLong;
    return u ? I.tUInt : I.tInt; // int / unsigned / signed
}

Entity Sema::typeEntityOf(Entity e) {
    if (e.kind != Entity::Kind::Decl) return e;
    const Decl* d = e.decl;
    switch (d->kind) {
    case DeclKind::Record:
    case DeclKind::Enum: {
        if (d->kind == DeclKind::Record && d->isTemplate && !d->isSpecialization) return e;
        Entity r;
        r.kind = Entity::Kind::Type;
        r.type = I.declType(d, I.nestedEnv(d, e.env));
        return r;
    }
    case DeclKind::Typedef: {
        if (d->isTemplate) return e;
        Entity r;
        r.kind = Entity::Kind::Type;
        r.type = I.resolveTypedef(d, e.env);
        return r;
    }
    default:
        return e;
    }
}

Entity Sema::parseName(Role role, bool soft) {
    const std::uint32_t save = p_;
    try {
        const std::uint8_t mask = role == Role::Value ? (kMaskValue | kMaskType | kMaskNamespace) : (kMaskType | kMaskNamespace);
        Entity scopeEnt;
        bool qualified = false;
        if (isP(pScope)) {
            ++p_;
            scopeEnt.kind = Entity::Kind::Decl;
            scopeEnt.decl = I.global;
            qualified = true;
        }
        Entity e;
        for (;;) {
            if (isK(kTemplate)) ++p_;
            if (!isIdent()) throw NotFound("expected a name");
            const Sym name = cur().sym;
            if (name < kFirstDynamicSym && name != 0) {
                // keywords never name entities (builtin type keywords are handled by the callers)
                throw NotFound("unexpected keyword '" + I.text(name) + "'");
            }
            ++p_;
            if (!qualified) {
                e = I.lookupUnqualified(name, sc_, mask);
            } else if (scopeEnt.kind == Entity::Kind::Decl && scopeEnt.decl->kind == DeclKind::Namespace) {
                e = I.lookupInNamespace(scopeEnt.decl, name, mask, 0);
            } else if (scopeEnt.kind == Entity::Kind::Type) {
                e = I.lookupInType(scopeEnt.type, name, mask);
            } else {
                throw NotFound("'" + I.text(name) + "': qualifier is not a namespace or class");
            }
            if (e.kind == Entity::Kind::None) throw NotFound("unknown name '" + I.text(name) + "'");
            if (e.kind == Entity::Kind::Decl && e.decl->isTemplate && !e.decl->isSpecialization &&
                (e.decl->kind == DeclKind::Record || e.decl->kind == DeclKind::Typedef) && isP(pLt)) {
                std::vector<TemplateArg> args = parseTemplateArgs();
                Entity t;
                t.kind = Entity::Kind::Type;
                t.type = e.decl->kind == DeclKind::Record ? I.instantiate(e.decl, e.env, std::move(args))
                                                          : I.instantiateAlias(e.decl, e.env, std::move(args));
                e = t;
            } else if (e.kind == Entity::Kind::Decl && e.decl->kind == DeclKind::Function && e.decl->isTemplate && isP(pLt)) {
                e.targs = parseTemplateArgs();
            } else if (e.kind == Entity::Kind::Type && e.decl && e.decl->kind == DeclKind::Record && e.decl->isTemplate && isP(pLt)) {
                // injected-class-name used as a template name
                std::vector<TemplateArg> args = parseTemplateArgs();
                Entity t;
                t.kind = Entity::Kind::Type;
                t.type = I.instantiate(e.decl, e.env, std::move(args));
                e = t;
            } else {
                e = typeEntityOf(e);
            }
            if (isP(pScope) && (at(p_ + 1).kind == static_cast<std::uint8_t>(TokKind::Ident) || isPunctSymAt(p_ + 1, pTilde))) {
                if (e.kind == Entity::Kind::Type || (e.kind == Entity::Kind::Decl && e.decl->kind == DeclKind::Namespace)) {
                    ++p_;
                    scopeEnt = e;
                    qualified = true;
                    continue;
                }
                if (e.kind == Entity::Kind::Decl && e.decl->isTemplate)
                    fail("template '" + I.text(e.decl->name) + "' used without arguments");
                throw NotFound("'" + I.text(name) + "' is not a scope");
            }
            break;
        }
        // validate against the role
        switch (role) {
        case Role::Type:
            if (e.kind == Entity::Kind::Decl && e.decl->isTemplate && (e.decl->kind == DeclKind::Record || e.decl->kind == DeclKind::Typedef))
                fail("template '" + I.text(e.decl->name) + "' used without template arguments");
            if (e.kind != Entity::Kind::Type) throw NotFound("not a type");
            break;
        case Role::Qualifier:
            if (!(e.kind == Entity::Kind::Type || (e.kind == Entity::Kind::Decl && e.decl->kind == DeclKind::Namespace))) throw NotFound("not a scope");
            break;
        case Role::Value:
            if (e.kind == Entity::Kind::Value) break;
            if (e.kind == Entity::Kind::Decl && (e.decl->kind == DeclKind::Variable || e.decl->kind == DeclKind::Enumerator || e.decl->kind == DeclKind::Field || e.decl->kind == DeclKind::Function)) break;
            throw NotFound("not a value");
        }
        return e;
    } catch (const NotFound&) {
        if (soft) {
            p_ = save;
            return Entity{};
        }
        throw;
    }
}

ConstValue Sema::entityValue(const Entity& e) {
    if (e.kind == Entity::Kind::Value) return e.value;
    if (e.kind == Entity::Kind::Decl) {
        switch (e.decl->kind) {
        case DeclKind::Variable: return I.variableValue(e.decl, e.env);
        case DeclKind::Enumerator: return I.enumeratorValue(e.decl, e.env);
        default: break;
        }
        fail("'" + I.text(e.decl->name) + "' is not a constant expression");
    }
    fail("not a constant expression");
}

TypeId Sema::parseTypeSpecifier() {
    for (;;) {
        if (!isIdent()) break;
        Sym s = cur().sym;
        if (s == kConst || s == kVolatile || s == kTypename || s == kStruct || s == kClass || s == kUnion || s == kEnum ||
            s == kCdecl || s == kStdcall) {
            ++p_;
            continue;
        }
        break;
    }
    TypeId t;
    if (isIdent() && isBuiltinTypeKeyword(cur().sym)) {
        t = parseBuiltin();
    } else {
        Entity e = parseName(Role::Type, false);
        t = e.type;
    }
    while (isK(kConst) || isK(kVolatile)) ++p_;
    return t;
}

TypeId Sema::parseAbstractDeclarator(TypeId base) {
    for (;;) {
        if (isP(pStar)) { base = I.pointerTo(base); ++p_; continue; }
        if (isP(pAmp) || isP(pAmpAmp)) { base = I.pointerTo(base); ++p_; continue; }
        if (isK(kConst) || isK(kVolatile) || isK(kPtr64) || isK(kRestrict)) { ++p_; continue; }
        break;
    }
    if (isP(pLParen) && (at(p_ + 1).sym == pStar || at(p_ + 1).sym == pAmp) && at(p_ + 1).kind == static_cast<std::uint8_t>(TokKind::Punct)) {
        // function pointer type: R (*)(args)
        std::uint32_t close = I.skipBalanced(p_, end_);
        p_ = close;
        if (isP(pLParen)) p_ = I.skipBalanced(p_, end_);
        return I.pointerTo(I.tVoid);
    }
    std::vector<std::uint64_t> counts;
    while (isP(pLBracket)) {
        std::uint32_t close = I.skipBalanced(p_, end_);
        Sema s(I, sc_, TokRange{p_ + 1, close - 1});
        ConstValue v = s.parseExpr();
        s.expectEnd();
        if (v.value < 0) fail("negative array extent");
        counts.push_back(static_cast<std::uint64_t>(v.value));
        p_ = close;
    }
    for (auto it = counts.rbegin(); it != counts.rend(); ++it) base = I.arrayOf(base, *it);
    return base;
}

TypeId Sema::parseTypeId() {
    SemaDepth depthGuard(depth_);
    TypeId t = parseTypeSpecifier();
    return parseAbstractDeclarator(t);
}

std::optional<TypeId> Sema::tryParseTypeId() {
    if (!isIdent()) return std::nullopt;
    const std::uint32_t save = p_;
    // Cheap pre-check: skip qualifiers, then the name must resolve as a type (soft lookup).
    std::uint32_t q = p_;
    while (q < end_ && I.tk[q].kind == static_cast<std::uint8_t>(TokKind::Ident) &&
           (I.tk[q].sym == kConst || I.tk[q].sym == kVolatile || I.tk[q].sym == kTypename || I.tk[q].sym == kStruct ||
            I.tk[q].sym == kClass || I.tk[q].sym == kUnion || I.tk[q].sym == kEnum))
        ++q;
    if (q >= end_) return std::nullopt;
    if (I.tk[q].kind == static_cast<std::uint8_t>(TokKind::Ident) && isBuiltinTypeKeyword(I.tk[q].sym)) {
        TypeId t = parseTypeId();
        return t;
    }
    p_ = q;
    Entity e = parseName(Role::Type, true);
    if (e.kind == Entity::Kind::None) {
        p_ = save;
        return std::nullopt;
    }
    while (isK(kConst) || isK(kVolatile)) ++p_;
    return parseAbstractDeclarator(e.type);
}

std::vector<TemplateArg> Sema::parseTemplateArgs() {
    SemaDepth depthGuard(depth_);
    std::vector<TemplateArg> args;
    expectP(pLt, "<");
    if (isP(pGt)) {
        ++p_;
        return args;
    }
    for (;;) {
        if (isP(pEllipsis)) fail("template parameter packs are not supported");
        TemplateArg a;
        const std::uint32_t save = p_;
        std::optional<TypeId> t;
        {
            NoGtGuard g(noGt_, true);
            t = tryParseTypeId();
        }
        if (t && (isP(pComma) || isP(pGt))) {
            a.isType = true;
            a.type = *t;
        } else {
            p_ = save;
            a.value = parseExprNoGt();
        }
        args.push_back(a);
        if (isP(pComma)) { ++p_; continue; }
        if (isP(pGt)) { ++p_; break; }
        fail("expected ',' or '>' in template argument list, found '" + I.tokenSpelling(p_) + "'");
    }
    return args;
}

// ---- expressions -----------------------------------------------------------------------------------------------

ConstValue Sema::parseExprNoGt() {
    NoGtGuard g(noGt_, true);
    return parseTernary();
}

ConstValue Sema::parseExpr() {
    NoGtGuard g(noGt_, false);
    return parseTernary();
}

ConstValue Sema::parseTernary() {
    ConstValue c = parseBinary(0);
    if (!isP(pQuestion)) return c;
    ++p_;
    const bool cond = c.value != 0;
    ConstValue a, b;
    {
        NoEvalGuard g(noEval_, !cond);
        NoGtGuard ng(noGt_, false);
        a = parseTernary();
    }
    expectP(pColon, ":");
    {
        NoEvalGuard g(noEval_, cond);
        b = parseTernary();
    }
    // usual arithmetic conversions between the branches
    if (a.bits == 1 && b.bits == 1) return cond ? a : b;
    std::uint8_t bits;
    bool sgn;
    commonType(a, b, bits, sgn);
    const ConstValue& pick = cond ? a : b;
    return withType(pick.value, bits, sgn);
}

bool Sema::peekBinary(int& prec, int& op, int& len) const {
    const Tk& t = cur();
    if (p_ >= end_ || t.kind != static_cast<std::uint8_t>(TokKind::Punct)) return false;
    len = 1;
    switch (t.sym) {
    case pPipePipe: prec = 1; op = OpLOr; return true;
    case pAmpAmp: prec = 2; op = OpLAnd; return true;
    case pPipe: prec = 3; op = OpOr; return true;
    case pCaret: prec = 4; op = OpXor; return true;
    case pAmp: prec = 5; op = OpAnd; return true;
    case pEq: prec = 6; op = OpEq; return true;
    case pNe: prec = 6; op = OpNe; return true;
    case pLt: prec = 7; op = OpLt; return true;
    case pLe: prec = 7; op = OpLe; return true;
    case pGe: prec = 7; op = OpGe; return !t.tight;
    case pGt: {
        const Tk& n = at(p_ + 1);
        if (n.tight && n.sym == pGt && p_ + 1 < end_) {
            prec = 8; op = OpShr; len = 2;
            return !noGt_;
        }
        if (n.tight && n.sym == pGe) return false;
        if (noGt_) return false;
        prec = 7; op = OpGt;
        return true;
    }
    case pShl: prec = 8; op = OpShl; return true;
    case pPlus: prec = 9; op = OpAdd; return true;
    case pMinus: prec = 9; op = OpSub; return true;
    case pStar: prec = 10; op = OpMul; return true;
    case pSlash: prec = 10; op = OpDiv; return true;
    case pPercent: prec = 10; op = OpMod; return true;
    default: return false;
    }
}

ConstValue Sema::parseBinary(int minPrec) {
    ConstValue lhs = parseUnary();
    for (;;) {
        int prec = 0, op = 0, len = 1;
        if (!peekBinary(prec, op, len) || prec < minPrec) break;
        p_ += static_cast<std::uint32_t>(len);
        if (op == OpLOr || op == OpLAnd) {
            const bool l = lhs.value != 0;
            const bool shortCircuit = (op == OpLOr) ? l : !l;
            NoEvalGuard g(noEval_, shortCircuit);
            ConstValue rhs = parseBinary(prec + 1);
            lhs = boolValue(op == OpLOr ? (l || rhs.value != 0) : (l && rhs.value != 0));
            continue;
        }
        ConstValue rhs = parseBinary(prec + 1);
        std::uint8_t bits;
        bool sgn;
        ConstValue res;
        if (op == OpShl || op == OpShr) {
            ConstValue l = promote(lhs);
            ConstValue r = promote(rhs);
            if (noEval_ == 0 && (r.value < 0 || r.value >= l.bits)) fail("invalid shift count");
            const int sh = (r.value < 0 || r.value >= l.bits) ? 0 : static_cast<int>(r.value);
            if (op == OpShl) {
                res = withType(static_cast<std::int64_t>(static_cast<std::uint64_t>(l.value) << sh), l.bits, l.isSigned);
            } else if (l.isSigned) {
                res = withType(l.value >> sh, l.bits, true);
            } else {
                res = withType(static_cast<std::int64_t>(static_cast<std::uint64_t>(l.value) >> sh), l.bits, false);
            }
            lhs = res;
            continue;
        }
        commonType(lhs, rhs, bits, sgn);
        const std::int64_t a = withType(lhs.value, bits, sgn).value;
        const std::int64_t b = withType(rhs.value, bits, sgn).value;
        const std::uint64_t ua = static_cast<std::uint64_t>(a);
        const std::uint64_t ub = static_cast<std::uint64_t>(b);
        switch (op) {
        case OpOr: res = withType(a | b, bits, sgn); break;
        case OpXor: res = withType(a ^ b, bits, sgn); break;
        case OpAnd: res = withType(a & b, bits, sgn); break;
        case OpEq: res = boolValue(a == b); break;
        case OpNe: res = boolValue(a != b); break;
        case OpLt: res = boolValue(sgn ? a < b : ua < ub); break;
        case OpLe: res = boolValue(sgn ? a <= b : ua <= ub); break;
        case OpGt: res = boolValue(sgn ? a > b : ua > ub); break;
        case OpGe: res = boolValue(sgn ? a >= b : ua >= ub); break;
        case OpAdd: res = withType(static_cast<std::int64_t>(ua + ub), bits, sgn); break;
        case OpSub: res = withType(static_cast<std::int64_t>(ua - ub), bits, sgn); break;
        case OpMul: res = withType(static_cast<std::int64_t>(ua * ub), bits, sgn); break;
        case OpDiv:
        case OpMod:
            if (b == 0) {
                if (noEval_ == 0) fail("division by zero in constant expression");
                res = withType(0, bits, sgn);
            } else if (sgn) {
                if (b == -1) res = withType(op == OpDiv ? static_cast<std::int64_t>(0 - ua) : 0, bits, sgn);
                else res = withType(op == OpDiv ? a / b : a % b, bits, sgn);
            } else {
                res = withType(static_cast<std::int64_t>(op == OpDiv ? ua / ub : ua % ub), bits, sgn);
            }
            break;
        default: res = lhs; break;
        }
        lhs = res;
    }
    return lhs;
}


ConstValue Sema::parseUnary() {
    SemaDepth depthGuard(depth_);
    const Tk& t = cur();
    if (p_ < end_ && t.kind == static_cast<std::uint8_t>(TokKind::Punct)) {
        switch (t.sym) {
        case pPlus: ++p_; return promote(parseUnary());
        case pMinus: {
            ++p_;
            ConstValue v = promote(parseUnary());
            return withType(static_cast<std::int64_t>(0 - static_cast<std::uint64_t>(v.value)), v.bits, v.isSigned);
        }
        case pTilde: {
            ++p_;
            ConstValue v = promote(parseUnary());
            return withType(~v.value, v.bits, v.isSigned);
        }
        case pBang: {
            ++p_;
            ConstValue v = parseUnary();
            return boolValue(v.value == 0);
        }
        default:
            break;
        }
    }
    return parsePrimary();
}

ConstValue Sema::castTo(const ConstValue& v, TypeId t) {
    const TypeKind k = I.types[t].layout.kind;
    if (k == TypeKind::Int || k == TypeKind::Char || k == TypeKind::Bool || k == TypeKind::Enum) return I.convertValue(v, t);
    fail("cast to non-integral type '" + I.types[t].layout.name + "' in constant expression");
}

ConstValue Sema::parseNumber(const Tk& tok) {
    std::string raw = I.src[tok.src].text;
    std::string s;
    for (char c : raw)
        if (c != '\'') s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    int base = 10;
    std::size_t pos = 0;
    if (s.size() > 1 && s[0] == '0') {
        if (s[1] == 'x') { base = 16; pos = 2; }
        else if (s[1] == 'b') { base = 2; pos = 2; }
        else if (std::isdigit(static_cast<unsigned char>(s[1]))) { base = 8; pos = 1; }
    }
    if (base != 16 && (s.find('.') != std::string::npos || (s.find('e') != std::string::npos)))
        fail("floating-point literal '" + raw + "' in constant expression");
    if (base == 16 && s.find('p') != std::string::npos) fail("floating-point literal '" + raw + "' in constant expression");
    if (base == 10 && s.find('.') != std::string::npos) fail("floating-point literal '" + raw + "' in constant expression");
    std::size_t digitsEnd = pos;
    auto isDigit = [&](char c) {
        if (base == 16) return std::isxdigit(static_cast<unsigned char>(c)) != 0;
        return c >= '0' && c < '0' + (base > 10 ? 10 : base);
    };
    while (digitsEnd < s.size() && isDigit(s[digitsEnd])) ++digitsEnd;
    std::string suffix = s.substr(digitsEnd);
    if (digitsEnd == pos && !(base == 8 && pos == 1)) fail("malformed integer literal '" + raw + "'");
    std::uint64_t value = 0;
    for (std::size_t i = pos; i < digitsEnd; ++i) {
        const char c = s[i];
        const unsigned d = static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : c - 'a' + 10);
        if (value > (UINT64_MAX - d) / static_cast<unsigned>(base)) fail("integer literal '" + raw + "' is too large");
        value = value * static_cast<unsigned>(base) + d;
    }
    bool uns = false;
    int longCount = 0;
    int msBits = 0;
    {
        std::size_t i = 0;
        while (i < suffix.size()) {
            if (suffix[i] == 'u') { uns = true; ++i; }
            else if (suffix[i] == 'l') { ++longCount; ++i; }
            else if (suffix[i] == 'i') {
                std::string num = suffix.substr(i + 1);
                if (num == "8") msBits = 8;
                else if (num == "16") msBits = 16;
                else if (num == "32") msBits = 32;
                else if (num == "64") msBits = 64;
                else fail("unknown integer suffix in '" + raw + "'");
                i = suffix.size();
            } else {
                fail("unknown integer suffix in '" + raw + "'");
            }
        }
    }
    const std::uint64_t v = value;
    const bool isDec = base == 10;
    const std::uint8_t L = I.opts.longIs64 ? 64 : 32;
    if (msBits) return withType(static_cast<std::int64_t>(v), static_cast<std::uint8_t>(msBits), !uns);
    struct Cand { std::uint8_t bits; bool sgn; };
    std::vector<Cand> cands;
    if (!uns) {
        if (longCount == 0) {
            cands.push_back({32, true});
            if (!isDec) cands.push_back({32, false});
        }
        if (longCount <= 1) {
            cands.push_back({L, true});
            if (!isDec) cands.push_back({L, false});
        }
        cands.push_back({64, true});
        cands.push_back({64, false});
    } else {
        if (longCount == 0) cands.push_back({32, false});
        if (longCount <= 1) cands.push_back({L, false});
        cands.push_back({64, false});
    }
    for (const Cand& c : cands) {
        const std::uint64_t maxv = c.sgn ? (c.bits == 64 ? static_cast<std::uint64_t>(INT64_MAX) : (1ull << (c.bits - 1)) - 1)
                                         : (c.bits == 64 ? UINT64_MAX : (1ull << c.bits) - 1);
        if (v <= maxv) return withType(static_cast<std::int64_t>(v), c.bits, c.sgn);
    }
    return withType(static_cast<std::int64_t>(v), 64, false);
}

ConstValue Sema::parseCharLit(const Tk& tok) {
    std::string raw = I.src[tok.src].text;
    std::size_t q = raw.find('\'');
    if (q == std::string::npos || raw.size() < q + 3) fail("malformed character literal");
    std::string prefix = raw.substr(0, q);
    std::int64_t v = 0;
    std::size_t i = q + 1;
    int n = 0;
    while (i < raw.size() && raw[i] != '\'') {
        std::int64_t c;
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            char e = raw[++i];
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
                while (i + 1 < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[i + 1]))) {
                    char h = raw[++i];
                    c = c * 16 + (std::isdigit(static_cast<unsigned char>(h)) ? h - '0' : std::tolower(h) - 'a' + 10);
                }
                break;
            }
            default:
                if (e >= '0' && e <= '7') {
                    c = e - '0';
                    for (int k = 0; k < 2 && i + 1 < raw.size() && raw[i + 1] >= '0' && raw[i + 1] <= '7'; ++k) c = c * 8 + (raw[++i] - '0');
                } else {
                    c = static_cast<unsigned char>(e);
                }
                break;
            }
        } else {
            c = static_cast<unsigned char>(raw[i]);
        }
        v = n == 0 ? c : (v << 8) | (c & 0xff);
        ++n;
        ++i;
    }
    if (prefix.empty()) return n > 1 ? withType(v, 32, true) : withType(v, 8, true);
    if (prefix == "L") return withType(v, I.opts.longIs64 ? 32 : 16, I.opts.longIs64);
    if (prefix == "u8") return withType(v, 8, false);
    if (prefix == "u") return withType(v, 16, false);
    return withType(v, 32, false);
}

std::optional<std::uint64_t> Sema::sizeOfDesignator() {
    const std::uint32_t save = p_;
    if (!isIdent() && !isP(pScope)) return std::nullopt;
    Entity e = parseName(Role::Value, true);
    if (e.kind != Entity::Kind::Decl || (e.decl->kind != DeclKind::Variable && e.decl->kind != DeclKind::Field)) {
        p_ = save;
        return std::nullopt;
    }
    TypeId t = I.typeOfDecl(e.decl, e.env);
    while (isP(pLBracket)) {
        const TypeLayout& l = I.layoutOf(t);
        if (l.kind != TypeKind::Array) { p_ = save; return std::nullopt; }
        t = l.element;
        p_ = I.skipBalanced(p_, end_);
    }
    if (isP(pDot) || isP(pArrow)) {
        p_ = save;
        return std::nullopt;
    }
    const TypeLayout& l = I.layoutOf(t);
    if (!l.complete) fail("sizeof of incomplete type '" + l.name + "'");
    return l.size;
}

ConstValue Sema::parseSizeof() {
    const bool isAlign = isK(kAlignof);
    ++p_;
    const std::uint32_t save = p_;
    auto resultFor = [&](const TypeLayout& l) {
        if (!l.complete) fail(std::string(isAlign ? "alignof" : "sizeof") + " of incomplete type '" + l.name + "': " + l.error);
        return withType(static_cast<std::int64_t>(isAlign ? l.align : l.size), 64, false);
    };
    if (isP(pLParen)) {
        ++p_;
        NoGtGuard g(noGt_, false);
        if (auto t = tryParseTypeId(); t && isP(pRParen)) {
            ++p_;
            return resultFor(I.layoutOf(*t));
        }
        p_ = save + 1;
        if (!isAlign) {
            if (auto sz = sizeOfDesignator(); sz && isP(pRParen)) {
                ++p_;
                return withType(static_cast<std::int64_t>(*sz), 64, false);
            }
            p_ = save + 1;
            ConstValue v = parseTernary();
            expectP(pRParen, ")");
            return withType(v.bits == 1 ? 1 : v.bits / 8, 64, false);
        }
        fail("alignof of an expression");
    }
    if (!isAlign) {
        if (auto sz = sizeOfDesignator()) return withType(static_cast<std::int64_t>(*sz), 64, false);
        ConstValue v = parseUnary();
        return withType(v.bits == 1 ? 1 : v.bits / 8, 64, false);
    }
    fail("expected '(' after alignof");
}

ConstValue Sema::parsePrimary() {
    if (p_ >= end_) fail("unexpected end of expression");
    const Tk& t = cur();
    switch (static_cast<TokKind>(t.kind)) {
    case TokKind::Number: {
        ++p_;
        return parseNumber(t);
    }
    case TokKind::CharLit: {
        ++p_;
        return parseCharLit(t);
    }
    case TokKind::StringLit:
        fail("string literal in constant expression");
    case TokKind::Punct: {
        if (t.sym == pLParen) {
            ++p_;
            NoGtGuard g(noGt_, false);
            if (auto ty = tryParseTypeId(); ty && isP(pRParen)) {
                ++p_;
                ConstValue v = parseUnary();
                return castTo(v, *ty);
            }
            ConstValue v = parseTernary();
            // comma operator
            while (isP(pComma)) { ++p_; v = parseTernary(); }
            expectP(pRParen, ")");
            return v;
        }
        fail("unexpected '" + I.tokenSpelling(p_) + "' in constant expression");
    }
    case TokKind::Ident:
        break;
    default:
        fail("unexpected end of expression");
    }

    switch (t.sym) {
    case kTrue: ++p_; return boolValue(true);
    case kFalse: ++p_; return boolValue(false);
    case kNullptr: ++p_; return withType(0, 64, false);
    case kSizeof:
    case kAlignof:
        return parseSizeof();
    case kStaticCast: case kReinterpretCast: case kConstCast: {
        const bool isStatic = t.sym == kStaticCast;
        ++p_;
        expectP(pLt, "<");
        TypeId ty;
        {
            NoGtGuard g(noGt_, true);
            ty = parseTypeId();
        }
        expectP(pGt, ">");
        expectP(pLParen, "(");
        ConstValue v;
        {
            NoGtGuard g(noGt_, false);
            v = parseTernary();
        }
        expectP(pRParen, ")");
        if (!isStatic && I.types[ty].layout.kind == TypeKind::Pointer) fail("pointer cast in constant expression");
        return castTo(v, ty);
    }
    default:
        break;
    }

    if (isBuiltinTypeKeyword(t.sym)) {
        TypeId ty = parseBuiltin();
        if (isP(pLParen)) {
            ++p_;
            NoGtGuard g(noGt_, false);
            ConstValue v = isP(pRParen) ? withType(0, 32, true) : parseTernary();
            expectP(pRParen, ")");
            return castTo(v, ty);
        }
        if (isP(pLBrace)) {
            ++p_;
            NoGtGuard g(noGt_, false);
            ConstValue v = isP(pRBrace) ? withType(0, 32, true) : parseTernary();
            expectP(pRBrace, "}");
            return castTo(v, ty);
        }
        fail("type name used as a value");
    }

    // locals of a constexpr function being evaluated
    if (!locals_.empty() && !isPunctSymAt(p_ + 1, pScope)) {
        for (auto it = locals_.rbegin(); it != locals_.rend(); ++it) {
            if (it->first == t.sym) {
                ++p_;
                return it->second;
            }
        }
    }

    // identifier-led: functional cast T(x) or a name
    const std::uint32_t save = p_;
    if (auto ty = tryParseTypeId()) {
        if (isP(pLParen) || isP(pLBrace)) {
            const bool brace = isP(pLBrace);
            ++p_;
            NoGtGuard g(noGt_, false);
            const Sym closer = brace ? pRBrace : pRParen;
            ConstValue v = isP(closer) ? withType(0, 32, true) : parseTernary();
            expectP(closer, brace ? "}" : ")");
            return castTo(v, *ty);
        }
        fail("type '" + I.types[*ty].layout.name + "' used as a value");
    }
    p_ = save;
    Entity e;
    try {
        e = parseName(Role::Value, false);
    } catch (const NotFound& ex) {
        if (noEval_ > 0) {
            // unevaluated branch: consume the name tokens and yield 0
            p_ = save;
            while (p_ < end_ && (isIdent() || isP(pScope))) {
                ++p_;
                if (isP(pLt)) {
                    std::uint32_t r = I.skipAngle(p_, end_);
                    if (r != UINT32_MAX) p_ = r;
                }
            }
            if (isP(pLParen)) p_ = I.skipBalanced(p_, end_);
            return withType(0, 32, true);
        }
        fail(ex.what());
    }
    if (isP(pLParen)) {
        // function call: constexpr functions with simple bodies
        const std::uint32_t open = p_;
        if (noEval_ > 0 || e.kind != Entity::Kind::Decl || e.decl->kind != DeclKind::Function) {
            if (noEval_ > 0) {
                p_ = I.skipBalanced(p_, end_);
                return withType(0, 32, true);
            }
            fail("call of '" + (e.kind == Entity::Kind::Decl ? I.text(e.decl->name) : std::string("?")) + "' in constant expression is not supported");
        }
        ++p_;
        std::vector<ConstValue> args;
        {
            NoGtGuard g(noGt_, false);
            while (!isP(pRParen)) {
                args.push_back(parseTernary());
                if (isP(pComma)) { ++p_; continue; }
                break;
            }
        }
        expectP(pRParen, ")");
        (void)open;
        return callFunction(e, std::move(args));
    }
    if (noEval_ > 0) {
        try {
            return entityValue(e);
        } catch (const SemaError&) {
            return withType(0, 32, true);
        }
    }
    return entityValue(e);
}

// ---- constexpr functions -------------------------------------------------------------------------------------

namespace {
TypeId typeForValue(Program::Impl& I, const ConstValue& v) {
    switch (v.bits) {
    case 1: return I.tBool;
    case 8: return v.isSigned ? I.tSChar : I.tUChar;
    case 16: return v.isSigned ? I.tShort : I.tUShort;
    case 32: return v.isSigned ? I.tInt : I.tUInt;
    default: return v.isSigned ? I.tLongLong : I.tULongLong;
    }
}
struct CallGuard {
    int& d;
    explicit CallGuard(int& x) : d(x) { ++d; }
    ~CallGuard() { --d; }
};
} // namespace

void Sema::skipStmt() {
    if (isP(pLBrace)) {
        p_ = I.skipBalanced(p_, end_);
        return;
    }
    if (isK(kIf)) {
        ++p_;
        if (isP(pLParen)) p_ = I.skipBalanced(p_, end_);
        skipStmt();
        if (isK(kElse)) {
            ++p_;
            skipStmt();
        }
        return;
    }
    while (p_ < end_ && !isP(pSemi)) {
        if (isP(pLParen) || isP(pLBracket) || isP(pLBrace)) p_ = I.skipBalanced(p_, end_);
        else ++p_;
    }
    if (isP(pSemi)) ++p_;
}

void Sema::execStmt() {
    if (returned_) {
        skipStmt();
        return;
    }
    if (isK(kReturn)) {
        ++p_;
        retVal_ = parseExpr();
        returned_ = true;
        if (isP(pSemi)) ++p_;
        return;
    }
    if (isP(pLBrace)) {
        ++p_;
        while (p_ < end_ && !isP(pRBrace)) execStmt();
        if (isP(pRBrace)) ++p_;
        return;
    }
    if (isK(kIf)) {
        ++p_;
        expectP(pLParen, "(");
        ConstValue c = parseExpr();
        expectP(pRParen, ")");
        if (c.value != 0) {
            execStmt();
            if (isK(kElse)) {
                ++p_;
                skipStmt();
            }
        } else {
            skipStmt();
            if (isK(kElse)) {
                ++p_;
                execStmt();
            }
        }
        return;
    }
    // local variable: [const|constexpr|static] type name = expr;
    const std::uint32_t save = p_;
    while (isK(kConst) || isK(kConstexpr) || isK(kStatic)) ++p_;
    bool isAuto = false;
    std::optional<TypeId> ty;
    if (isK(kAuto)) {
        isAuto = true;
        ++p_;
    } else {
        ty = tryParseTypeId();
    }
    if ((ty || isAuto) && isIdent() && at(p_ + 1).kind == static_cast<std::uint8_t>(TokKind::Punct) && at(p_ + 1).sym == pAssign) {
        const Sym name = cur().sym;
        p_ += 2;
        ConstValue v = parseExpr();
        if (ty && I.isIntegral(*ty)) v = I.convertValue(v, *ty);
        locals_.emplace_back(name, v);
        if (isP(pSemi)) ++p_;
        return;
    }
    p_ = save;
    fail("unsupported statement in constexpr function body");
}

ConstValue Sema::callFunction(const Entity& fn, std::vector<ConstValue> args) {
    const Decl* fd = fn.decl;
    const Decl* pick = nullptr;
    if (fd->parent) {
        auto it = fd->parent->byName.find(fd->name);
        if (it != fd->parent->byName.end())
            for (const Decl* c : it->second)
                if (c->kind == DeclKind::Function && c->fparams.size() == args.size()) pick = c;
    }
    if (!pick) fail("no matching overload of '" + I.text(fd->name) + "' for " + std::to_string(args.size()) + " argument(s)");
    fd = pick;
    if (I.callDepth >= 64) fail("constexpr call depth exceeded in '" + I.text(fd->name) + "'");
    CallGuard guard(I.callDepth);

    I.envs.emplace_back();
    Env* env = &I.envs.back();
    env->outer = fn.env;
    env->decl = fd;
    const Scope psc{fd, env, true};
    for (std::size_t i = 0; i < fd->params.size(); ++i) {
        const TemplateParam& tp = fd->params[i];
        TemplateArg a;
        bool have = false;
        if (i < fn.targs.size()) {
            a = fn.targs[i];
            have = true;
        } else if (tp.isType) {
            for (std::size_t j = 0; j < fd->fparams.size() && j < args.size(); ++j) {
                const FuncParam& fp = fd->fparams[j];
                if (fp.type.e == fp.type.b + 1 && I.tk[fp.type.b].sym == tp.name && fp.ptrDepth == 0) {
                    a.isType = true;
                    a.type = typeForValue(I, args[j]);
                    have = true;
                    break;
                }
            }
        }
        if (!have && tp.hasDefault) {
            Sema s(I, psc, tp.defaultRange);
            if (tp.isType) {
                a.isType = true;
                a.type = s.parseTypeId();
            } else {
                a.value = s.parseExprNoGt();
            }
            have = true;
        }
        if (!have) fail("cannot deduce template argument '" + I.text(tp.name) + "' of '" + I.text(fd->name) + "'");
        a = I.convertArg(tp, a, psc);
        env->args.push_back(a);
    }
    const Scope fsc{fd, env, false};
    Sema body(I, fsc, fd->body);
    for (std::size_t j = 0; j < fd->fparams.size(); ++j) {
        const FuncParam& fp = fd->fparams[j];
        TypeSpec ts;
        ts.tokens = fp.type;
        ts.ptrDepth = fp.ptrDepth;
        ts.isRef = fp.isRef;
        TypeId pt = I.resolveTypeSpec(ts, fsc);
        ConstValue v = args[j];
        if (I.isIntegral(pt) && !fp.isRef) v = I.convertValue(v, pt);
        body.locals_.emplace_back(fp.name, v);
    }
    while (!body.atEnd() && !body.returned_) body.execStmt();
    if (!body.returned_) fail("constexpr function '" + I.text(fd->name) + "' did not return a value");
    ConstValue r = body.retVal_;
    TypeId rt = I.resolveTypeSpec(fd->type, fsc);
    if (I.isIntegral(rt)) r = I.convertValue(r, rt);
    return r;
}

// ---- initializers ----------------------------------------------------------------------------------------------

void Sema::skipInitElement() {
    while (p_ < end_) {
        const Tk& t = cur();
        if (t.kind == static_cast<std::uint8_t>(TokKind::Punct)) {
            if (t.sym == pComma || t.sym == pRBrace) return;
            if (t.sym == pLParen || t.sym == pLBracket || t.sym == pLBrace) { p_ = I.skipBalanced(p_, end_); continue; }
            if (t.sym == pLt && I.angleOpens(p_)) {
                std::uint32_t r = I.skipAngle(p_, end_);
                if (r != UINT32_MAX) { p_ = r; continue; }
            }
        }
        ++p_;
    }
}

std::string Sema::parseStringLiteral() {
    std::string s;
    while (p_ < end_ && cur().kind == static_cast<std::uint8_t>(TokKind::StringLit)) {
        s += I.decodeStringLiteral(p_);
        ++p_;
    }
    return s;
}

InitValue Sema::parseInitScalar(TypeId type) {
    InitValue v;
    v.type = type;
    const std::uint32_t start = p_;
    const TypeKind kind = type == kNoType ? TypeKind::Void : I.types[type].layout.kind;
    try {
        if (cur().kind == static_cast<std::uint8_t>(TokKind::StringLit)) {
            v.kind = InitValue::Kind::Str;
            v.s = parseStringLiteral();
            return v;
        }
        if (isP(pDot) || isP(pLBracket)) fail("designated initializers are not supported");
        if (kind == TypeKind::Float) fail("floating-point initializer");
        ConstValue c = parseTernary();
        if (type != kNoType && I.isIntegral(type)) c = I.convertValue(c, type);
        v.kind = InitValue::Kind::Int;
        v.i = c.value;
        v.isSigned = c.isSigned;
        return v;
    } catch (const SemaError& ex) {
        p_ = start;
        skipInitElement();
        v = InitValue{};
        v.type = type;
        v.kind = InitValue::Kind::Unknown;
        v.error = ex.what();
        return v;
    }
}

InitValue Sema::parseInitAggregate(TypeId type, bool braced) {
    const TypeLayout layout = I.layoutOf(type); // copy: nested layouts may add types
    InitValue out;
    out.kind = InitValue::Kind::List;
    out.type = type;
    std::vector<std::pair<std::string, TypeId>> slots;
    std::uint64_t limit = 0;
    if (layout.kind == TypeKind::Array) {
        limit = layout.count;
    } else {
        for (const FieldLayout& f : layout.fields) slots.emplace_back(f.name, f.type);
        limit = layout.recordKind == RecordKind::Union ? std::min<std::size_t>(1, slots.size()) : slots.size();
        for (std::uint64_t i = 0; i < limit; ++i) out.names.push_back(slots[i].first);
    }
    std::uint64_t i = 0;
    while (p_ < end_ && i < limit) {
        if (isP(pRBrace)) break;
        TypeId et = layout.kind == TypeKind::Array ? layout.element : slots[i].second;
        out.items.push_back(parseInit(et, false));
        ++i;
        if (isP(pComma)) { ++p_; continue; }
        break;
    }
    if (braced) {
        // surplus elements
        while (p_ < end_ && !isP(pRBrace)) {
            skipInitElement();
            if (isP(pComma)) ++p_;
            else break;
        }
    }
    if (layout.kind == TypeKind::Record) out.names.resize(out.items.size());
    return out;
}

InitValue Sema::parseInit(TypeId type, bool braced) {
    (void)braced;
    if (type == kNoType) {
        InitValue v;
        v.error = "unknown type";
        skipInitElement();
        return v;
    }
    const TypeLayout& l0 = I.layoutOf(type);
    const TypeKind kind = l0.kind;
    const bool aggregate = kind == TypeKind::Array || kind == TypeKind::Record;
    if (kind == TypeKind::Array && cur().kind == static_cast<std::uint8_t>(TokKind::StringLit) &&
        I.types[l0.element].layout.kind == TypeKind::Char) {
        InitValue v;
        v.kind = InitValue::Kind::Str;
        v.type = type;
        v.s = parseStringLiteral();
        return v;
    }
    if (aggregate && isP(pLBrace)) {
        ++p_;
        InitValue v = parseInitAggregate(type, true);
        if (isP(pRBrace)) ++p_;
        return v;
    }
    if (aggregate) return parseInitAggregate(type, false);
    if (isP(pLBrace)) { // scalar in braces
        ++p_;
        InitValue v = parseInitScalar(type);
        if (isP(pRBrace)) ++p_;
        return v;
    }
    return parseInitScalar(type);
}

} // namespace qstate::cpp
