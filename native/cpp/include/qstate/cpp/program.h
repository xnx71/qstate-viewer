// Declaration parser + constant evaluator + template instantiation + layout engine.
//
// Input: the token stream of an ALREADY PREPROCESSED translation unit (qstate::cpp::PreprocessResult::tokens).
// The parser is tolerant: it records namespaces, records (struct / class / union), enums, typedefs / aliases,
// class templates (with specializations), variables and static_asserts, and skips everything else (function
// bodies, member functions, constructors, friends, ...) by bracket matching. Types are resolved, templates are
// instantiated and layouts are computed lazily on demand, so only what a query needs is ever evaluated.
//
// Layout model: x86-64, natural alignment, MSVC rules (members of a derived class start at sizeof(Base) rounded
// to the member alignment, empty bases take no space, bit-fields are allocated like MSVC). No type is special
// cased: m256i, uint128_t, QPI::Array, QPI::HashMap, ... are ordinary types laid out from their parsed members.
//
// Thread-safety: a Program is NOT thread-safe (queries mutate caches). Use one instance per thread or lock.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "qstate/cpp/diag.h"
#include "qstate/cpp/preprocessor.h"
#include "qstate/cpp/token.h"
#include "qstate/cpp/types.h"

namespace qstate::cpp {

class Program {
public:
    ~Program();
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    // Parses `tokens` (must end with an End token; Token::file indexes `files`). Parse problems are appended to
    // `diags` (and kept in Program::diags()). Never throws for malformed input.
    static std::unique_ptr<Program> parse(std::vector<Token> tokens, std::vector<std::string> files, Diags& diags,
                                          ProgramOptions options = {});

    const ProgramOptions& options() const;
    const std::vector<std::string>& files() const;
    std::string fileName(std::uint32_t file) const;

    // ---- types -------------------------------------------------------------------------------------------------
    // Resolves a (possibly qualified, possibly template-id) type name, e.g. "QX::StateData" or
    // "QPI::Array<QPI::id, 8>". `scope` is the qualified name of the namespace / record the lookup starts in
    // ("" = global namespace). Returns kNoType (and appends a diagnostic) when it cannot be resolved.
    TypeId lookupType(std::string_view qualifiedName, std::string_view scope = {});

    // Layout of a type (computed on first use, cached; the reference stays valid for the Program's lifetime).
    // Never throws: a failed layout has complete == false and an `error`.
    const TypeLayout& layoutOf(TypeId id);

    // Canonical display name ("QPI::Collection<QX::AssetOrder, 2097152>").
    std::string typeName(TypeId id);

    // Every type created so far (builtins, pointers, arrays, records, instantiations), for debugging.
    std::vector<TypeId> allTypes() const;
    std::size_t typeCount() const;

    // ---- constants ---------------------------------------------------------------------------------------------
    // Evaluates an integral constant expression such as "sizeof(QX::StateData)" or "NUMBER_OF_COMPUTORS * 2" in the
    // scope `scope` (qualified name of a namespace or record, "" = global). Failures are returned as nullopt with
    // the reason in `error` (when given) and appended to diags().
    std::optional<ConstValue> evalConstant(std::string_view expression, std::string_view scope = {},
                                           std::string* error = nullptr);
    std::optional<ConstValue> evalConstant(const std::vector<Token>& tokens, std::string_view scope = {},
                                           std::string* error = nullptr);

    // ---- variables ---------------------------------------------------------------------------------------------
    // Namespace-scope (or static member) variable by (qualified) name, e.g. "contractDescriptions".
    std::optional<VariableInfo> lookupVariable(std::string_view qualifiedName, std::string_view scope = {});
    // All variables declared directly in a namespace / record (debug listing and bulk access).
    std::vector<VariableInfo> variables(std::string_view scope = {});
    // Evaluates the initializer of a variable against its type: arrays and structs become Lists (arrays of
    // unknown extent get their extent from the initializer), char arrays become Str, scalars Int (sizeof and
    // friends go through the layout engine). Elements that cannot be evaluated become Kind::Unknown.
    std::optional<InitValue> evalInitializer(const VariableInfo& variable);

    // Text of the token range [begin, end) of the (preprocessed) input, joined by single spaces.
    std::string tokenText(std::uint32_t begin, std::uint32_t end) const;

    // ---- introspection -----------------------------------------------------------------------------------------
    // All named declarations (namespaces, records, enums, typedefs, variables); templates flagged.
    std::vector<DeclInfo> declarations() const;
    const Diags& diags() const;
    // Number of top-level tokens consumed / declarations seen (statistics for tests).
    std::size_t declarationCount() const;

    struct Impl;

private:
    Program();
    std::unique_ptr<Impl> impl_;
};

// Converts the `#pragma pack(push, n) / pack(pop) / pack(n) / pack()` records of a preprocessor run into regions
// for ProgramOptions::packRegions. `endToken` = number of tokens of the stream.
std::vector<PackRegion> packRegionsFromPragmas(const std::vector<PragmaRecord>& pragmas, std::uint32_t endToken);

} // namespace qstate::cpp
