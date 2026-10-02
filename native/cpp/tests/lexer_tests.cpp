#include <doctest/doctest.h>

#include "qstate/cpp/token.h"

using namespace qstate::cpp;

namespace {
std::vector<std::string> texts(const std::vector<Token>& v) {
    std::vector<std::string> r;
    for (const Token& t : v)
        if (t.kind != TokKind::End) r.push_back(t.text);
    return r;
}
} // namespace

TEST_CASE("lexer: basic tokens and punctuators") {
    auto t = lex("a::b<c>>=d; x->y ... <=>");
    CHECK(texts(t) == std::vector<std::string>{"a", "::", "b", "<", "c", ">>=", "d", ";", "x", "->", "y", "...", "<=>"});
    CHECK(t.back().kind == TokKind::End);
}

TEST_CASE("lexer: numbers keep suffixes, exponents and separators") {
    auto t = lex("1ULL 0x7fffffffffffffffi64 1'000'000 1.5e+3 0x1p-3 2097152*X");
    CHECK(texts(t) == std::vector<std::string>{"1ULL", "0x7fffffffffffffffi64", "1'000'000", "1.5e+3", "0x1p-3", "2097152", "*", "X"});
    CHECK(t[0].kind == TokKind::Number);
}

TEST_CASE("lexer: hex numbers do not swallow a following minus after e") {
    auto t = lex("0xE-1");
    CHECK(texts(t) == std::vector<std::string>{"0xE", "-", "1"});
}

TEST_CASE("lexer: comments, splices and line tracking") {
    auto t = lex("a /* x\ny */ b // c\n#define X \\\n  1\nz");
    REQUIRE(t.size() == 8);
    CHECK(t[0].text == "a");
    CHECK(t[1].text == "b");
    CHECK(t[1].line == 2);
    CHECK(!t[1].atLineStart);
    CHECK(t[2].text == "#");
    CHECK(t[2].atLineStart);
    CHECK(t[2].line == 3);
    CHECK(t[5].text == "1");
    CHECK(!t[5].atLineStart); // spliced into the directive line
    CHECK(t[6].text == "z");
    CHECK(t[6].atLineStart);
    CHECK(t[6].line == 5);
}

TEST_CASE("lexer: literals") {
    auto t = lex("'a' L'b' \"s\\\"x\" u8\"q\" R\"d(raw)\" )d\" '\\n'");
    CHECK(t[0].kind == TokKind::CharLit);
    CHECK(t[1].text == "L'b'");
    CHECK(t[2].text == "\"s\\\"x\"");
    CHECK(t[3].text == "u8\"q\"");
    CHECK(t[4].kind == TokKind::StringLit);
    CHECK(t[4].text == "R\"d(raw)\" )d\"");
    CHECK(t[5].text == "'\\n'");
}

TEST_CASE("lexer: spaceBefore flag") {
    auto t = lex("f(x) f (x)");
    CHECK(!t[1].spaceBefore);
    CHECK(t[4].spaceBefore);
}
