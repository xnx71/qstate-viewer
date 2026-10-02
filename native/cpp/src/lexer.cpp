#include "qstate/cpp/token.h"

#include <array>
#include <cctype>

namespace qstate::cpp {
namespace {

bool isIdentStart(unsigned char c) { return std::isalpha(c) || c == '_' || c == '$' || c >= 0x80; }
bool isIdentChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

constexpr std::array<std::string_view, 5> kPunct3 = {"<<=", ">>=", "...", "->*", "<=>"};
constexpr std::array<std::string_view, 22> kPunct2 = {"::", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&",
                                                      "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", ".*", "##"};

class Lexer {
public:
    Lexer(std::string_view text, std::uint32_t file, std::uint32_t firstLine)
        : text_(text), file_(file), line_(firstLine) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        while (true) {
            skipSpace();
            if (pos_ >= text_.size()) break;
            out.push_back(next());
        }
        return out;
    }

private:
    char peek(std::size_t ahead = 0) const { return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0'; }

    // Consumes a backslash-newline pair if present at pos_; returns true when consumed.
    bool spliceAt() {
        if (peek() != '\\') return false;
        std::size_t p = pos_ + 1;
        if (p < text_.size() && text_[p] == '\r') ++p;
        if (p < text_.size() && text_[p] == '\n') {
            pos_ = p + 1;
            ++line_;
            return true;
        }
        return false;
    }

    void skipSpace() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == '\n') {
                ++pos_;
                ++line_;
                lineStart_ = true;
                sawSpace_ = true;
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                ++pos_;
                sawSpace_ = true;
            } else if (spliceAt()) {
                sawSpace_ = true;
            } else if (c == '/' && peek(1) == '/') {
                while (pos_ < text_.size() && text_[pos_] != '\n') {
                    if (!spliceAt()) ++pos_;
                }
                sawSpace_ = true;
            } else if (c == '/' && peek(1) == '*') {
                pos_ += 2;
                while (pos_ < text_.size() && !(text_[pos_] == '*' && peek(1) == '/')) {
                    if (text_[pos_] == '\n') ++line_;
                    ++pos_;
                }
                pos_ = pos_ < text_.size() ? pos_ + 2 : pos_;
                sawSpace_ = true;
            } else {
                break;
            }
        }
    }

    // Reads the characters of a token, transparently skipping line splices.
    void take(std::string& s) {
        s.push_back(text_[pos_++]);
    }

    Token next() {
        Token t;
        t.file = file_;
        t.line = line_;
        t.atLineStart = lineStart_;
        t.spaceBefore = sawSpace_;
        lineStart_ = false;
        sawSpace_ = false;

        const unsigned char c = static_cast<unsigned char>(peek());

        if (std::isdigit(c) || (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
            t.kind = TokKind::Number;
            while (pos_ < text_.size()) {
                const char d = text_[pos_];
                if ((d == '+' || d == '-') && !t.text.empty()) {
                    const char prev = t.text.back();
                    const bool hex = t.text.size() > 1 && t.text[0] == '0' && (t.text[1] == 'x' || t.text[1] == 'X');
                    if ((!hex && (prev == 'e' || prev == 'E')) || prev == 'p' || prev == 'P') {
                        take(t.text);
                        continue;
                    }
                    break;
                }
                if (std::isalnum(static_cast<unsigned char>(d)) || d == '_' || d == '.') {
                    take(t.text);
                } else if (d == '\'' && pos_ + 1 < text_.size() && std::isalnum(static_cast<unsigned char>(text_[pos_ + 1]))) {
                    take(t.text); // digit separator
                } else if (!spliceAt()) {
                    break;
                }
            }
            return t;
        }

        if (isIdentStart(c)) {
            std::string word;
            while (pos_ < text_.size() && (isIdentChar(static_cast<unsigned char>(text_[pos_])) || peek() == '\\')) {
                if (peek() == '\\') {
                    if (!spliceAt()) break;
                    continue;
                }
                take(word);
            }
            // String / char literal with an encoding prefix.
            const char q = peek();
            const bool prefix = word == "L" || word == "u" || word == "U" || word == "u8";
            const bool rawPrefix = word == "R" || word == "LR" || word == "uR" || word == "UR" || word == "u8R";
            if ((q == '"' || q == '\'') && prefix) return literal(t, std::move(word));
            if (q == '"' && rawPrefix) return rawString(t, std::move(word));
            t.kind = TokKind::Ident;
            t.text = std::move(word);
            return t;
        }

        if (c == '"' || c == '\'') return literal(t, {});

        t.kind = TokKind::Punct;
        for (std::string_view p : kPunct3) {
            if (text_.compare(pos_, 3, p) == 0) {
                t.text = std::string(p);
                pos_ += 3;
                return t;
            }
        }
        for (std::string_view p : kPunct2) {
            if (text_.compare(pos_, 2, p) == 0) {
                t.text = std::string(p);
                pos_ += 2;
                return t;
            }
        }
        take(t.text);
        return t;
    }

    Token literal(Token t, std::string prefix) {
        const char quote = peek();
        t.kind = quote == '"' ? TokKind::StringLit : TokKind::CharLit;
        t.text = std::move(prefix);
        take(t.text);
        while (pos_ < text_.size()) {
            const char d = text_[pos_];
            if (d == '\n') break; // unterminated literal: stop at end of line
            if (d == '\\') {
                if (spliceAt()) continue;
                take(t.text);
                if (pos_ < text_.size() && text_[pos_] != '\n') take(t.text);
                continue;
            }
            take(t.text);
            if (d == quote) break;
        }
        return t;
    }

    Token rawString(Token t, std::string prefix) {
        t.kind = TokKind::StringLit;
        t.text = std::move(prefix);
        take(t.text); // opening quote
        std::string delim;
        while (pos_ < text_.size() && peek() != '(' && peek() != '\n') {
            delim.push_back(peek());
            take(t.text);
        }
        const std::string close = ")" + delim + "\"";
        while (pos_ < text_.size()) {
            if (text_.compare(pos_, close.size(), close) == 0) {
                t.text.append(close);
                pos_ += close.size();
                break;
            }
            if (text_[pos_] == '\n') ++line_;
            take(t.text);
        }
        return t;
    }

    std::string_view text_;
    std::uint32_t file_;
    std::uint32_t line_;
    std::size_t pos_ = 0;
    bool lineStart_ = true;
    bool sawSpace_ = false;
};

} // namespace

std::vector<Token> lexFragment(std::string_view text, std::uint32_t file, std::uint32_t firstLine) {
    return Lexer(text, file, firstLine).run();
}

std::vector<Token> lex(std::string_view text, std::uint32_t file) {
    std::vector<Token> out = Lexer(text, file, 1).run();
    Token end;
    end.kind = TokKind::End;
    end.file = file;
    end.line = out.empty() ? 1 : out.back().line;
    end.atLineStart = true;
    out.push_back(std::move(end));
    return out;
}

} // namespace qstate::cpp
