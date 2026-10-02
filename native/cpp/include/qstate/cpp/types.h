// Public result types of the declaration parser / constant evaluator / layout engine (see program.h).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qstate::cpp {

// Canonical type identity: the same type (including the same template instantiation) always has the same id.
using TypeId = std::uint32_t;
constexpr TypeId kNoType = 0xFFFFFFFFu;

enum class TypeKind : std::uint8_t {
    Void,
    Bool,
    Char,    // char, signed/unsigned char, wchar_t, char8_t, char16_t, char32_t (see `isSigned`)
    Int,     // short, int, long, long long and their unsigned variants
    Float,   // float, double, long double
    NullPtr,
    Pointer, // pointers, references, function pointers (all 8 bytes)
    Array,
    Record,  // struct / class / union
    Enum,
};

enum class RecordKind : std::uint8_t { Struct, Class, Union };

struct SourceLoc {
    std::uint32_t file = 0; // index into Program::files()
    std::uint32_t line = 0;
};

struct FieldLayout {
    std::string name;         // empty for anonymous struct / union members
    TypeId type = kNoType;
    std::string declaredType; // spelling of the declared type as written (typedef names kept): "Array<id, 8>"
    std::uint64_t offset = 0; // from the start of the record; for bit-fields: byte offset of the storage unit
    std::uint64_t size = 0;
    std::uint32_t align = 1;
    // Bit-fields only (bitWidth > 0): bit position inside the storage unit counted from the LSB.
    std::uint32_t bitOffset = 0;
    std::uint32_t bitWidth = 0;
    SourceLoc loc;
};

struct BaseLayout {
    TypeId type = kNoType;
    std::uint64_t offset = 0;
};

struct EnumeratorInfo {
    std::string name;
    std::int64_t value = 0;
};

struct TemplateArgInfo {
    bool isType = false;
    TypeId type = kNoType;     // when isType
    std::int64_t value = 0;    // otherwise
    std::string text;          // rendered: "QX::AssetOrder", "2097152", "true"
};

struct TypeLayout {
    TypeId id = kNoType;
    TypeKind kind = TypeKind::Void;
    RecordKind recordKind = RecordKind::Struct;
    std::string name;     // canonical display name, e.g. "QPI::Collection<QX::AssetOrder, 2097152>"
    std::string baseName; // unqualified name of the declaration ("Collection", "PoV", "" for anonymous)
    std::uint64_t size = 0;
    std::uint32_t align = 1;
    bool complete = false; // false: layout failed or the type is incomplete (see `error`, Program::diags())
    std::string error;

    bool isSigned = false;     // Int / Char / enum underlying type
    bool isEmpty = false;      // record without data members and without non-empty bases

    std::vector<FieldLayout> fields; // non-static data members in declaration order
    std::vector<BaseLayout> bases;

    // Enum.
    TypeId underlying = kNoType;
    bool scopedEnum = false;
    std::vector<EnumeratorInfo> enumerators;

    // Array / Pointer.
    TypeId element = kNoType;
    std::uint64_t count = 0;

    // Records instantiated from a class template.
    std::string templateName; // qualified name of the primary template, "QPI::Collection"; empty otherwise
    std::vector<TemplateArgInfo> templateArgs;
    TypeId enclosing = kNoType; // lexically enclosing record (nested types), kNoType otherwise

    SourceLoc loc; // definition
};

// Result of a constant evaluation. `value` holds the bits sign- or zero-extended according to `isSigned`.
struct ConstValue {
    std::int64_t value = 0;
    bool isSigned = true;
    std::uint8_t bits = 32; // 1 (bool), 8, 16, 32 or 64

    std::uint64_t asUnsigned() const { return static_cast<std::uint64_t>(value); }
};

// Evaluated initializer of a variable (see Program::evalInitializer).
struct InitValue {
    enum class Kind : std::uint8_t { Int, Str, List, Unknown };
    Kind kind = Kind::Unknown;
    std::int64_t i = 0;
    bool isSigned = true;
    std::string s;                  // Str: decoded string literal contents (without terminator)
    std::vector<InitValue> items;   // List: array elements / struct members in declaration order
    std::vector<std::string> names; // List of a struct: member names parallel to `items` (empty for arrays)
    TypeId type = kNoType;          // type the initializer was matched against (kNoType if unknown)
    std::string error;              // Unknown: why
};

struct VariableInfo {
    std::uint32_t id = 0;           // opaque handle for Program::evalInitializer
    std::string name;
    std::string qualifiedName;
    TypeId type = kNoType;          // kNoType when the type could not be resolved
    bool isConstexpr = false;
    bool isStatic = false;
    bool isExtern = false;
    bool hasInitializer = false;
    std::uint32_t initBegin = 0;    // token index range [initBegin, initEnd) into Program::tokenText
    std::uint32_t initEnd = 0;
    SourceLoc loc;
};

// A named declaration (debug listing).
struct DeclInfo {
    enum class Kind : std::uint8_t { Namespace, Record, Enum, Typedef, Variable, Function };
    Kind kind = Kind::Record;
    std::string qualifiedName;
    bool isTemplate = false;
    bool isDefined = false;
    SourceLoc loc;
};

// "#pragma pack" state of a stretch of the token stream: records whose definition starts in [beginToken, endToken)
// (indices into the token vector given to Program::parse) are laid out with maximum member alignment `pack`.
struct PackRegion {
    std::uint32_t beginToken = 0;
    std::uint32_t endToken = 0;
    std::uint32_t pack = 0; // 1, 2, 4, 8, 16
};

struct ProgramOptions {
    // Size of `long` / `unsigned long` (and, with it, wchar_t = 4). The Qubic node is built with MSVC for UEFI
    // (LLP64: long = 4, wchar_t = 2), which is also what its state files use; no state type contains `long`.
    // Set true for LP64 (g++/clang on Linux).
    bool longIs64 = false;
    // Evaluate namespace-scope and non-template class-scope static_assert declarations after parsing and report
    // failures as diagnostics.
    bool checkStaticAsserts = true;
    // #pragma pack regions (see packRegionsFromPragmas in program.h); empty = no packing anywhere.
    std::vector<PackRegion> packRegions;
    // Limits (recursion).
    int maxInstantiationDepth = 64;
};

} // namespace qstate::cpp
