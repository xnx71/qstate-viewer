// Declaration tree produced by the tolerant declaration parser (internal to the front-end, exposed for tests).
// Only declarations that matter for types and constants are represented; type expressions, initializers and
// template arguments stay as token ranges and are interpreted lazily (with name lookup) by the evaluator.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "qstate/cpp/types.h"

namespace qstate::cpp {

// Interned identifier / punctuator.
using Sym = std::uint32_t;

// Well known symbols; their values are fixed (interned first, in this order).
#define QSTATE_SYMS(X)                                                                                                \
    X(None, "")                                                                                                       \
    X(kStruct, "struct") X(kClass, "class") X(kUnion, "union") X(kEnum, "enum") X(kNamespace, "namespace")            \
    X(kUsing, "using") X(kTypedef, "typedef") X(kTemplate, "template") X(kTypename, "typename")                      \
    X(kStatic, "static") X(kConstexpr, "constexpr") X(kConst, "const") X(kVolatile, "volatile")                       \
    X(kInline, "inline") X(kExtern, "extern") X(kFriend, "friend") X(kPublic, "public") X(kPrivate, "private")        \
    X(kProtected, "protected") X(kStaticAssert, "static_assert") X(kSizeof, "sizeof") X(kAlignof, "alignof")          \
    X(kAlignas, "alignas") X(kOperator, "operator") X(kVirtual, "virtual") X(kExplicit, "explicit")                   \
    X(kMutable, "mutable") X(kRegister, "register") X(kThreadLocal, "thread_local") X(kDecltype, "decltype")          \
    X(kAuto, "auto") X(kUnsigned, "unsigned") X(kSigned, "signed") X(kShort, "short") X(kLong, "long")                \
    X(kInt, "int") X(kChar, "char") X(kBool, "bool") X(kVoid, "void") X(kFloat, "float") X(kDouble, "double")         \
    X(kWchar, "wchar_t") X(kChar8, "char8_t") X(kChar16, "char16_t") X(kChar32, "char32_t")                           \
    X(kInt8, "__int8") X(kInt16, "__int16") X(kInt32, "__int32") X(kInt64, "__int64") X(kTrue, "true")                \
    X(kFalse, "false") X(kNullptr, "nullptr") X(kStaticCast, "static_cast") X(kReinterpretCast, "reinterpret_cast")   \
    X(kConstCast, "const_cast") X(kDynamicCast, "dynamic_cast") X(kFinal, "final") X(kOverride, "override")           \
    X(kDefault, "default") X(kDelete, "delete") X(kNoexcept, "noexcept") X(kThis, "this") X(kNew, "new")              \
    X(kThrow, "throw") X(kDeclspec, "__declspec") X(kAttribute, "__attribute__") X(kCdecl, "__cdecl")                 \
    X(kStdcall, "__stdcall") X(kPtr64, "__ptr64") X(kRestrict, "__restrict") X(kForceinline, "__forceinline")         \
    X(kAsm, "asm") X(kConstinit, "constinit") X(kConsteval, "consteval") X(kInlineNs, "__inline") X(kReturn, "return") X(kIf, "if") X(kElse, "else")    \
    X(pLBrace, "{") X(pRBrace, "}") X(pLParen, "(") X(pRParen, ")") X(pLBracket, "[") X(pRBracket, "]")              \
    X(pLt, "<") X(pGt, ">") X(pScope, "::") X(pColon, ":") X(pSemi, ";") X(pComma, ",") X(pStar, "*")                 \
    X(pAmp, "&") X(pAmpAmp, "&&") X(pAssign, "=") X(pDot, ".") X(pEllipsis, "...") X(pPlus, "+") X(pMinus, "-")      \
    X(pSlash, "/") X(pPercent, "%") X(pCaret, "^") X(pPipe, "|") X(pTilde, "~") X(pBang, "!") X(pQuestion, "?")      \
    X(pShl, "<<") X(pLe, "<=") X(pGe, ">=") X(pEq, "==") X(pNe, "!=") X(pPipePipe, "||") X(pArrow, "->")             \
    X(pHash, "#")

enum : Sym {
#define QSTATE_SYM_ENUM(name, text) name,
    QSTATE_SYMS(QSTATE_SYM_ENUM)
#undef QSTATE_SYM_ENUM
    kFirstDynamicSym
};

struct TokRange {
    std::uint32_t b = 0;
    std::uint32_t e = 0;
    bool empty() const { return b >= e; }
};

struct Decl;

// A declared type as written: decl-specifier tokens + declarator modifiers.
struct TypeSpec {
    TokRange tokens;               // decl-specifier-seq without storage class / cv noise handled by the resolver
    Decl* inlineDecl = nullptr;    // struct / union / enum defined inline in the declaration
    std::uint8_t ptrDepth = 0;
    bool isRef = false;
    bool isFuncPtr = false;        // pointer to function (declarator "(*name)(...)" or a function typedef)
    bool unsized = false;          // outermost extent is empty "[]" (extent comes from the initializer)
    bool isAuto = false;
    std::vector<TokRange> extents; // array extents, outermost first
};

struct TemplateParam {
    Sym name = 0;
    bool isType = true;            // typename / class parameter (also template template parameters)
    bool isPack = false;
    TokRange typeRange;            // NTTP: its type
    bool hasDefault = false;
    TokRange defaultRange;
};

enum class DeclKind : std::uint8_t { Namespace, Record, Enum, Enumerator, Typedef, Variable, Field, StaticAssert, Function };

// Parameter of a constexpr function (only constexpr functions with a body are recorded).
struct FuncParam {
    Sym name = 0;
    TokRange type;
    std::uint8_t ptrDepth = 0;
    bool isRef = false;
};

struct Decl {
    DeclKind kind = DeclKind::Namespace;
    Sym name = 0;
    Decl* parent = nullptr;
    std::uint32_t id = 0;
    std::uint32_t file = 0;
    std::uint32_t line = 0;
    std::uint32_t srcTok = 0; // index of the declaration's first token in the original token vector
    TokRange alignExpr;       // alignas(...) / __declspec(align(...)) on this declaration

    // flags
    bool isStatic = false;
    bool isConstexpr = false;
    bool isConst = false;
    bool isExtern = false;
    bool defined = false;      // record / enum: has a body
    bool isTemplate = false;   // class template (primary or specialization), alias template
    bool isSpecialization = false; // explicit or partial specialization of `primary`
    bool scoped = false;       // enum class
    bool hasUnderlying = false;
    bool isBitField = false;
    bool hasInit = false;
    bool hasValue = false;     // enumerator with explicit value
    bool anonymous = false;    // unnamed struct / union / enum
    bool isFunctionTypedef = false;
    RecordKind recordKind = RecordKind::Struct;

    // templates
    std::vector<TemplateParam> params;
    Decl* primary = nullptr;               // specialization: its primary template
    std::vector<TokRange> specArgs;        // specialization: the arguments after the name
    std::vector<Decl*> specs;              // primary: all specializations

    // scopes (namespace / record / enum)
    std::vector<Decl*> members;                          // declaration order
    std::unordered_map<Sym, std::vector<Decl*>> byName;  // lookup table
    std::vector<Decl*> usings;                           // using-directives (namespaces)
    std::vector<TokRange> bases;                         // base class type ranges

    // typedef / variable / field
    TypeSpec type;
    TokRange init;
    TokRange bitWidth;
    // enum
    TokRange underlying;
    // enumerator
    TokRange value;
    Decl* prevEnumerator = nullptr; // previous enumerator of the same enum (implicit value = previous + 1)
    // static_assert
    TokRange expr;
    // constexpr function (kind Function): `type` is the return type, `params` the template parameters
    std::vector<FuncParam> fparams;
    TokRange body;
};

} // namespace qstate::cpp
