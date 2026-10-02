// Token model shared by the lexer, the preprocessor and the declaration parser.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qstate::cpp {

enum class TokKind : std::uint8_t {
    Ident,     // identifiers and keywords (keywords are not distinguished here)
    Number,    // pp-number: 12, 0x1F, 1ULL, 0x7fffffffffffffffi64, 1'000, 1.5e+3
    CharLit,   // 'a', L'a', u8'a' (text includes prefix and quotes, escapes unprocessed)
    StringLit, // "abc", L"abc", u8"abc", R"(raw)" (text includes prefix and quotes)
    Punct,     // operators and punctuation; ">>" and ">>=" are single tokens (the parser splits them)
    End,       // end of input marker; always the last token of a lexed buffer / preprocessed output
};

struct Token {
    TokKind kind = TokKind::End;
    std::string text;
    // Index into the file table of the owning buffer / PreprocessResult::files.
    std::uint32_t file = 0;
    // 1-based line where the token starts. For tokens produced by macro expansion: the line of the
    // outermost macro invocation.
    std::uint32_t line = 0;
    // First token of a logical line (after line splicing). Directives are detected through this flag.
    bool atLineStart = false;
    // Whitespace or a comment preceded the token (needed for stringizing and function-like macro detection).
    bool spaceBefore = false;

    bool is(TokKind k) const { return kind == k; }
    bool isPunct(std::string_view p) const { return kind == TokKind::Punct && text == p; }
    bool isIdent(std::string_view s) const { return kind == TokKind::Ident && text == s; }
};

// Splits `text` into tokens. Comments are dropped, backslash-newline is spliced. A trailing End token is
// appended. Never throws: unknown bytes become single-character Punct tokens.
// `file` is stored into every token.
std::vector<Token> lex(std::string_view text, std::uint32_t file = 0);

// Same, but without the trailing End token (used by the preprocessor for macro bodies / argument text).
std::vector<Token> lexFragment(std::string_view text, std::uint32_t file = 0, std::uint32_t firstLine = 1);

} // namespace qstate::cpp
