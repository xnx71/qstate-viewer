// Internal state of qstate::cpp::Program: token arena, symbol table, declaration tree, type table, caches.
// Implemented across program.cpp (glue, symbols), parser.cpp (declarations), lookup.cpp (names, templates,
// instantiation), sema.cpp (type and constant expression evaluation), layout.cpp (layout engine).
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "qstate/cpp/ast.h"
#include "qstate/cpp/program.h"

namespace qstate::cpp {

// Compact token used by the parser (parallel to the original Token vector).
struct Tk {
    Sym sym = 0;            // identifiers and punctuators; 0 for literals / End
    std::uint8_t kind = 0;  // TokKind
    std::uint8_t tight = 0; // second half of a split ">>" / ">>="
    std::uint32_t src = 0;  // index into Impl::src
};

struct SemaError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// Name lookup found nothing suitable (soft failure while probing "type or expression").
struct NotFound : SemaError {
    using SemaError::SemaError;
};

enum class Role : std::uint8_t { Type, Value, Qualifier };

struct TemplateArg {
    bool isType = false;
    TypeId type = kNoType;
    ConstValue value;
};

// Instantiation environment of a record / alias scope: binds the template parameters of `decl`.
struct Env {
    const Env* outer = nullptr; // environment of the lexically enclosing scope
    const Decl* decl = nullptr;
    std::vector<TemplateArg> args; // parallel to decl->params
    std::vector<TemplateArg> primaryArgs; // specializations: the arguments of the primary template (display)
    TypeId self = kNoType;
};

struct Scope {
    const Decl* decl = nullptr;
    const Env* env = nullptr;
    bool paramsOnly = false; // only the template parameters of `decl` are visible at the first level
};

struct Entity {
    enum class Kind : std::uint8_t { None, Decl, Type, Value };
    Kind kind = Kind::None;
    const Decl* decl = nullptr; // Kind::Decl
    const Env* env = nullptr;   // environment of decl->parent
    TypeId type = kNoType;      // Kind::Type
    ConstValue value;           // Kind::Value
    std::vector<TemplateArg> targs; // function templates: explicit template arguments
};

struct TypeInfo {
    TypeLayout layout;
    const Decl* decl = nullptr;
    const Env* env = nullptr;
    enum class State : std::uint8_t { None, InProgress, Done } state = State::None;
    std::uint8_t basesState = 0; // 0 unresolved, 1 resolving, 2 done
    std::vector<TypeId> bases;
};

struct PtrPairHash {
    std::size_t operator()(const std::pair<const void*, const void*>& p) const noexcept {
        return std::hash<const void*>()(p.first) * 1000003u ^ std::hash<const void*>()(p.second);
    }
};

struct Program::Impl {
    explicit Impl(ProgramOptions o);

    ProgramOptions opts;
    std::vector<std::string> files;
    std::vector<Token> src;
    std::vector<Tk> tk;
    Diags diagList;

    // symbols
    std::unordered_map<std::string, Sym> symMap;
    std::vector<std::string> symText;
    Sym intern(std::string_view s);
    const std::string& text(Sym s) const { return symText[s]; }

    // tokens: appends (splitting ">>" / ">>=") and returns the range [b, e) of the new Tk entries; the End token
    // of the input is not appended.
    TokRange appendTokens(std::vector<Token>&& tokens);
    std::string tokenSpelling(std::uint32_t i) const; // original text of Tk i
    std::string rangeText(TokRange r) const;          // tokens joined with spacing heuristics

    // declarations
    std::deque<Decl> decls;
    Decl* global = nullptr;
    std::unordered_set<Sym> templateNames;
    std::unordered_set<Sym> valueNames; // variables / fields / enumerators / template parameters
    Decl* newDecl(DeclKind kind, Sym name, Decl* parent, std::uint32_t tkIndex);
    void addMember(Decl* scope, Decl* d, bool registerName);
    std::string qualifiedName(const Decl* d) const;

    // diagnostics
    void diag(Diag::Severity sev, std::string msg, std::uint32_t tkIndex);
    void diagAt(Diag::Severity sev, std::string msg, std::uint32_t file, std::uint32_t line);

    // token skipping helpers shared by the parser and the initializer scanner
    // all return the index just past the construct
    std::uint32_t skipBalanced(std::uint32_t p, std::uint32_t end) const; // p at ( [ { ; returns past the match
    // p at '<'; returns index past the matching '>' or UINT32_MAX when unbalanced. `strict`: nested '<' only
    // open after known template names.
    std::uint32_t skipAngle(std::uint32_t p, std::uint32_t end) const;
    bool angleOpens(std::uint32_t p) const; // token p is '<' after a known template name

    // ---- types -------------------------------------------------------------------------------------------------
    std::deque<TypeInfo> types;
    TypeId tVoid = kNoType, tBool = kNoType, tChar = kNoType, tSChar = kNoType, tUChar = kNoType, tShort = kNoType,
           tUShort = kNoType, tInt = kNoType, tUInt = kNoType, tLong = kNoType, tULong = kNoType,
           tLongLong = kNoType, tULongLong = kNoType, tFloat = kNoType, tDouble = kNoType, tLongDouble = kNoType,
           tWchar = kNoType, tChar8 = kNoType, tChar16 = kNoType, tChar32 = kNoType, tNullPtr = kNoType;
    void initBuiltins();
    TypeId newType(TypeKind kind);
    TypeId addBuiltin(TypeKind kind, std::uint32_t size, bool isSigned, const char* name);
    TypeId pointerTo(TypeId t);
    TypeId arrayOf(TypeId t, std::uint64_t count);
    std::unordered_map<TypeId, TypeId> pointerCache;
    std::map<std::pair<TypeId, std::uint64_t>, TypeId> arrayCache;

    // environments of non-template nested records inside template instances
    std::deque<Env> envs;
    std::map<std::pair<const Decl*, const Env*>, const Env*> nestedEnvs;
    // Environment for a record / enum `d` (no own parameters) declared inside the scope with environment `outer`.
    const Env* nestedEnv(const Decl* d, const Env* outer);
    // Environment for the scope `d` found in a scope whose env is `e` (d->parent chain aware): d is an enum or
    // record nested in the scope that `e` describes.
    TypeId declType(const Decl* d, const Env* env);
    std::unordered_map<std::pair<const void*, const void*>, TypeId, PtrPairHash> declTypes;
    std::unordered_map<std::string, TypeId> instances;
    std::unordered_map<std::string, TypeId> rawInstances;
    int instDepth = 0;
    int callDepth = 0;
    int layoutDepth = 0;

    TypeId instantiate(const Decl* primary, const Env* outer, std::vector<TemplateArg> args);
    TypeId instantiateAlias(const Decl* alias, const Env* outer, std::vector<TemplateArg> args);
    void completeArgs(const Decl* tmpl, const Env* outer, std::vector<TemplateArg>& args);
    bool matchSpecialization(const Decl* spec, const Env* outer, const std::vector<TemplateArg>& args,
                             std::vector<TemplateArg>& bound);
    TemplateArg convertArg(const TemplateParam& p, TemplateArg a, Scope sc);
    std::string argsKey(const std::vector<TemplateArg>& args) const;
    std::string renderArg(const TemplateArg& a);
    std::string scopeDisplay(const Decl* d, const Env* e);

    // conversions
    ConstValue convertValue(const ConstValue& v, TypeId t) const;
    static ConstValue normalize(std::int64_t v, std::uint8_t bits, bool isSigned);

    // ---- lookup ------------------------------------------------------------------------------------------------
    Entity lookupUnqualified(Sym name, Scope sc, std::uint8_t mask);
    Entity lookupInDecl(const Decl* d, const Env* e, Sym name, std::uint8_t mask, int depth);
    Entity lookupInNamespace(const Decl* ns, Sym name, std::uint8_t mask, int depth);
    Entity lookupInType(TypeId t, Sym name, std::uint8_t mask);
    const std::vector<TypeId>& basesOf(TypeId t);
    Entity declEntity(const Decl* d, const Env* envOfScopeFound, const Decl* foundIn);

    TypeId resolveTypedef(const Decl* d, const Env* env);
    TypeId resolveTypeSpec(const TypeSpec& spec, Scope sc, const Decl* owner = nullptr);
    TypeId typeOfDecl(const Decl* d, const Env* env); // variables / fields (arrays of unknown extent included)
    ConstValue variableValue(const Decl* d, const Env* env);
    ConstValue enumeratorValue(const Decl* d, const Env* env);
    std::map<std::pair<const Decl*, const Env*>, TypeId> typedefCache;
    std::map<std::pair<const Decl*, const Env*>, ConstValue> valueCache;
    std::set<std::pair<const Decl*, const Env*>> inProgress;
    std::uint64_t countInitElements(TokRange init, TypeId elem);
    std::string decodeStringLiteral(std::uint32_t tkIndex) const;

    // ---- layout ------------------------------------------------------------------------------------------------
    const TypeLayout& layoutOf(TypeId id);
    void computeLayout(TypeId id);
    void computeRecord(TypeId id);
    void computeEnum(TypeId id);
    bool isIntegral(TypeId t) const;
    void checkStaticAsserts();
    void checkStaticAssertsIn(const Decl* scope);

    // ---- scope by name -----------------------------------------------------------------------------------------
    bool scopeFromName(std::string_view name, Scope& out, std::string& err);

    // statistics
    std::size_t declCount = 0;
};

// Interprets token ranges: type-ids, template arguments, constant expressions, initializers.
class Sema {
public:
    Sema(Program::Impl& impl, Scope sc, TokRange r) : I(impl), sc_(sc), p_(r.b), end_(r.e) {}

    std::uint32_t pos() const { return p_; }
    bool atEnd() const { return p_ >= end_; }

    TypeId parseTypeId();
    std::optional<TypeId> tryParseTypeId(); // restores the position when the tokens are not a type
    ConstValue parseExpr();                 // conditional-expression (no comma operator)
    ConstValue parseExprNoGt();             // inside template arguments: a top-level '>' ends the expression
    std::vector<TemplateArg> parseTemplateArgs(); // at '<'
    Entity parseName(Role role, bool soft);       // qualified name; soft: unresolved -> Kind::None
    InitValue parseInit(TypeId type, bool braced);
    ConstValue entityValue(const Entity& e);
    void expectEnd();
    // Calls the constexpr function `fn` (found in a scope with environment e.env).
    ConstValue callFunction(const Entity& fn, std::vector<ConstValue> args);

private:
    Program::Impl& I;
    Scope sc_;
    std::uint32_t p_;
    std::uint32_t end_;
    int noEval_ = 0;
    int depth_ = 0;
    bool noGt_ = false;
    std::vector<std::pair<Sym, ConstValue>> locals_;
    bool returned_ = false;
    ConstValue retVal_;
    void execStmt();
    void skipStmt();

    const Tk& cur() const;
    const Tk& at(std::uint32_t i) const;
    bool isP(Sym s) const { return p_ < end_ && cur().sym == s && cur().kind == static_cast<std::uint8_t>(TokKind::Punct); }
    bool isK(Sym s) const { return p_ < end_ && cur().sym == s && cur().kind == static_cast<std::uint8_t>(TokKind::Ident); }
    bool isIdent() const;
    bool isPunctSymAt(std::uint32_t i, Sym s) const { return i < end_ && I.tk[i].kind == static_cast<std::uint8_t>(TokKind::Punct) && I.tk[i].sym == s; }
    [[noreturn]] void fail(const std::string& msg) const;
    void expectP(Sym s, const char* what);

    TypeId parseTypeSpecifier();
    bool isBuiltinTypeKeyword(Sym s) const;
    TypeId parseBuiltin();
    TypeId parseAbstractDeclarator(TypeId base);
    Entity typeEntityOf(Entity e); // Decl Record / Typedef -> Kind::Type
    TypeId entityType(const Entity& e);

    ConstValue parseTernary();
    ConstValue parseBinary(int minPrec);
    ConstValue parseUnary();
    ConstValue parsePrimary();
    ConstValue parseSizeof();
    ConstValue parseNumber(const Tk& t);
    ConstValue parseCharLit(const Tk& t);
    ConstValue castTo(const ConstValue& v, TypeId t);
    bool peekBinary(int& prec, int& op, int& len) const;
    std::optional<std::uint64_t> sizeOfDesignator();
    void skipInitElement();
    InitValue parseInitScalar(TypeId type);
    InitValue parseInitAggregate(TypeId type, bool braced);
    std::string parseStringLiteral();
};

} // namespace qstate::cpp
