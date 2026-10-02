// C/C++ preprocessor: directives, conditional compilation, object and function-like macros (#, ##, __VA_ARGS__),
// #include resolution through a SourceProvider. Output is ONE flat, fully expanded token vector.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "qstate/cpp/diag.h"
#include "qstate/cpp/source.h"
#include "qstate/cpp/token.h"

namespace qstate::cpp {

struct PreprocessOptions {
    // Directories (relative to the source root, "" = the root itself) searched for `#include <...>` and, after the
    // including file's own directory, for `#include "..."`. Example: {"src", ""}.
    std::vector<std::string> includeDirs;

    // Predefined object-like macros, e.g. {"NO_UEFI", "1"}, {"_MSC_VER", "1944"}.
    std::vector<std::pair<std::string, std::string>> defines;

    // Includes that cannot be resolved (system headers such as <immintrin.h>) are skipped silently when true,
    // otherwise a Warning diagnostic is produced. A missing quoted include is always a warning.
    bool silentSystemIncludes = true;

    // Safety limits.
    int maxIncludeDepth = 200;
    int maxExpansionDepth = 256;
    // Budget against runaway (exponential) macro expansion: preprocessing stops with an Error diagnostic and
    // PreprocessResult::aborted when more output tokens than this would be produced. The real Qubic translation
    // unit needs about 600 thousand.
    std::size_t maxOutputTokens = 16'000'000;
};

// A `#pragma` directive that was removed from the token stream (except `once` and push_macro/pop_macro, which the
// preprocessor consumes). `tokenIndex` is the number of output tokens produced before the directive, so the
// parser can apply e.g. `pack(push, 1)` / `pack(pop)` at the right position of the stream.
struct PragmaRecord {
    std::string text; // everything after "#pragma ", tokens joined by single spaces where the source had whitespace
    std::uint32_t file = 0;
    std::uint32_t line = 0;
    std::size_t tokenIndex = 0;
};

struct PreprocessResult {
    // Fully expanded tokens (directives removed), terminated by one End token. Token::file indexes `files`.
    std::vector<Token> tokens;
    // Every file that was read, normalized relative paths, in first-read order.
    std::vector<std::string> files;
    Diags diags;
    // `#pragma` directives in stream order (see PragmaRecord).
    std::vector<PragmaRecord> pragmas;
    // True when a safety limit (expansion budget) stopped preprocessing early; `tokens` is then incomplete.
    bool aborted = false;
    // Macro table after the run (name -> replacement text, for diagnostics / tests); function-like macros are
    // rendered as "NAME(params) body".
    std::vector<std::pair<std::string, std::string>> finalMacros;
};

class Preprocessor {
public:
    Preprocessor(const SourceProvider& source, PreprocessOptions options);

    // Preprocesses the translation unit rooted at `path` (relative to the source root).
    // Never throws for malformed input: problems become diagnostics.
    PreprocessResult run(std::string_view path);

private:
    const SourceProvider& source_;
    PreprocessOptions options_;
};

} // namespace qstate::cpp
