#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "qstate/cpp/preprocessor.h"
#include "test_env.h"

using namespace qstate::cpp;

namespace {

struct Run {
    PreprocessResult result;
    // Token texts joined by single spaces (End excluded).
    std::string text;
    bool hasDiag(Diag::Severity sev, const std::string& needle = {}) const {
        for (const Diag& d : result.diags)
            if (d.severity == sev && d.message.find(needle) != std::string::npos) return true;
        return false;
    }
    std::size_t count(Diag::Severity sev) const {
        std::size_t n = 0;
        for (const Diag& d : result.diags) n += d.severity == sev;
        return n;
    }
};

std::string joinTokens(const std::vector<Token>& tokens) {
    std::string s;
    for (const Token& t : tokens) {
        if (t.kind == TokKind::End) continue;
        if (!s.empty()) s += ' ';
        s += t.text;
    }
    return s;
}

Run preprocess(std::map<std::string, std::string> files, const std::string& root = "main.h",
               std::vector<std::pair<std::string, std::string>> defines = {}, std::vector<std::string> dirs = {""}) {
    MapSource src(std::move(files));
    PreprocessOptions opt;
    opt.includeDirs = std::move(dirs);
    opt.defines = std::move(defines);
    Run r;
    r.result = Preprocessor(src, opt).run(root);
    r.text = joinTokens(r.result.tokens);
    return r;
}

// Preprocesses a single in-memory file.
std::string pp(const std::string& code, std::vector<std::pair<std::string, std::string>> defines = {}) {
    return preprocess({{"main.h", code}}, "main.h", std::move(defines)).text;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// output shape

TEST_CASE("pp: plain tokens pass through and the stream ends with End") {
    Run r = preprocess({{"main.h", "int x = 1;\nstruct S { int a; };\n"}});
    CHECK(r.text == "int x = 1 ; struct S { int a ; } ;");
    REQUIRE(!r.result.tokens.empty());
    CHECK(r.result.tokens.back().kind == TokKind::End);
    CHECK(r.result.diags.empty());
    CHECK(r.result.files == std::vector<std::string>{"main.h"});
    CHECK(!r.result.aborted);
}

TEST_CASE("pp: empty input and missing root file") {
    Run r = preprocess({{"main.h", ""}});
    REQUIRE(r.result.tokens.size() == 1);
    CHECK(r.result.tokens[0].kind == TokKind::End);
    Run m = preprocess({}, "nope.h");
    REQUIRE(m.result.tokens.size() == 1);
    CHECK(m.hasDiag(Diag::Severity::Error, "cannot open"));
}

TEST_CASE("pp: tokens keep file index and line, directives vanish") {
    Run r = preprocess({{"main.h", "#define A 1\n\nint a;\n#include \"x.h\"\nint b;\n"}, {"x.h", "int x;\n\n\nint y;\n"}});
    REQUIRE(r.result.files.size() == 2);
    CHECK(r.result.files[1] == "x.h");
    const auto& t = r.result.tokens;
    // int a ; int x ; int y ; int b ; End
    REQUIRE(t.size() == 13);
    CHECK(t[0].text == "int");
    CHECK((t[0].file == 0 && t[0].line == 3));
    CHECK((t[3].file == 1 && t[3].line == 1));
    CHECK((t[6].file == 1 && t[6].line == 4));
    CHECK((t[9].file == 0 && t[9].line == 5));
    CHECK(t[0].atLineStart);
    CHECK(!t[1].atLineStart);
}

TEST_CASE("pp: macro expanded tokens get the line of the outermost invocation") {
    Run r = preprocess({{"main.h", "#define ONE 1\n#define TWO ONE + ONE\n\n\nx = TWO;\ny = f(\n  TWO,\n  3);\n#define f(a, b) a b\n"}});
    // x = 1 + 1 ; y = f ( ... ) ; (f not yet defined at the second use)
    const auto& t = r.result.tokens;
    REQUIRE(t.size() > 6);
    CHECK(t[0].line == 5);
    for (int i = 2; i <= 4; ++i) CHECK(t[i].line == 5); // 1 + 1
    CHECK(t[5].line == 5);                              // ;
}

TEST_CASE("pp: macro invoked across lines keeps the invocation line for the result") {
    Run r = preprocess({{"main.h", "#define f(a, b) a b\nint\nf(\n  p,\n  q\n) z;\n"}});
    CHECK(r.text == "int p q z ;");
    const auto& t = r.result.tokens;
    CHECK(t[0].line == 2);
    CHECK(t[1].line == 3);
    CHECK(t[2].line == 3);
    CHECK(t[3].line == 6);
}

// ---------------------------------------------------------------------------------------------------------------------
// object-like macros

TEST_CASE("pp: object-like macros, chains, undef and redefinition") {
    CHECK(pp("#define A B\n#define B 2\nA A") == "2 2");
    CHECK(pp("#define A 1\nA\n#undef A\nA") == "1 A");
    CHECK(pp("#define A 1\n#undef A\n#define A 2\nA") == "2");
    CHECK(pp("#define EMPTY\n[EMPTY] [ EMPTY EMPTY ]") == "[ ] [ ]");
    CHECK(pp("#define A  a   b\t c  \nA") == "a b c");
    CHECK(pp("#undef NEVER_DEFINED\nx") == "x");
}

TEST_CASE("pp: incompatible redefinition warns, identical redefinition is silent") {
    Run same = preprocess({{"main.h", "#define A 1 + 2\n#define A 1 + 2\nA"}});
    CHECK(same.result.diags.empty());
    CHECK(same.text == "1 + 2");
    Run diff = preprocess({{"main.h", "#define A 1\n#define A 2\nA"}});
    CHECK(diff.hasDiag(Diag::Severity::Warning, "redefined"));
    CHECK(diff.text == "2");
}

TEST_CASE("pp: self-referential macros are not expanded recursively (painted blue)") {
    CHECK(pp("#define foo foo bar\nfoo") == "foo bar");
    CHECK(pp("#define obj (obj + 1)\nobj") == "( obj + 1 )");
    CHECK(pp("#define AA BB\n#define BB AA\nAA BB") == "AA BB");
    CHECK(pp("#define X1 X2\n#define X2 X1 + 1\nX1 X2") == "X1 + 1 X2 + 1");
}

TEST_CASE("pp: predefined macros come from options.defines") {
    CHECK(pp("A B", {{"A", "1"}, {"B", ""}}) == "1");
    CHECK(pp("#ifdef FOO\nyes\n#else\nno\n#endif", {{"FOO", "1"}}) == "yes");
    CHECK(pp("#ifdef FOO\nyes\n#else\nno\n#endif") == "no");
    CHECK(pp("SQ(3)", {{"SQ(x)", "((x)*(x))"}}) == "( ( 3 ) * ( 3 ) )");
    CHECK(pp("V", {{"V", "a b + 1"}}) == "a b + 1");
}

// ---------------------------------------------------------------------------------------------------------------------
// function-like macros

TEST_CASE("pp: function-like macros, arguments and spacing") {
    CHECK(pp("#define ADD(a, b) ((a) + (b))\nADD(1, 2) ADD( 3 , ADD(4,5) )") == "( ( 1 ) + ( 2 ) ) ( ( 3 ) + ( ( ( 4 ) + ( 5 ) ) ) )");
    CHECK(pp("#define F(x) [x]\nF((1,2)) F(a(b,c))") == "[ ( 1 , 2 ) ] [ a ( b , c ) ]");
    CHECK(pp("#define Z() zero\nZ() Z( )") == "zero zero");
    CHECK(pp("#define F(x) x\nF") == "F");
    CHECK(pp("#define F(x) x\nF + 1") == "F + 1");
    CHECK(pp("#define F(x) x\n#define G F\nG(2) G (3)") == "2 3");
}

TEST_CASE("pp: nested function-like macros and argument pre-expansion") {
    CHECK(pp("#define NEST(x) [x]\n#define NEST2(x) NEST(NEST(x))\nNEST2(NEST2(a))") == "[ [ [ [ a ] ] ] ]");
    CHECK(pp("#define ID(x) x\nID(ID(ID(7)))") == "7");
    CHECK(pp("#define A() 1\n#define B A\nB() B ( ) B") == "1 1 A");
    CHECK(pp("#define x 3\n#define f(a) f(x * (a))\n#undef x\n#define x 2\n#define g f\n#define z z[0]\n#define h g(~\n#define m(a) a(w)\n#define w 0,1\n"
             "#define t(a) a\n#define p() int\n#define q(x) x\n#define r(x,y) x ## y\n#define str(x) # x\n"
             "f(y+1) + f(f(z)) % t(t(g)(0) + t)(1);\n") == "f ( 2 * ( y + 1 ) ) + f ( 2 * ( f ( 2 * ( z [ 0 ] ) ) ) ) % f ( 2 * ( 0 ) ) + t ( 1 ) ;");
    // function-like macro whose name is produced by an expansion and called with arguments from outside
    CHECK(pp("#define f(x) x f\n#define g f(1)\ng(2) f(3)(4)") == "1 f ( 2 ) 3 f ( 4 )");
    CHECK(pp("#define ID(x) x\n#define C(x) x\nID(ID)(1) ID(C)(3)") == "ID ( 1 ) 3"); // painted ID is not re-expanded
    CHECK(pp("#define LPAREN (\n#define RPAREN )\n#define F(x, y) x + y\n#define ELLIP_FUNC(...) __VA_ARGS__\nELLIP_FUNC(F, LPAREN, 'a', 'b', RPAREN);") ==
          "F , ( , 'a' , 'b' , ) ;");
    CHECK(pp("#define F1(x) F2(x) x\n#define F2(x) F1(x) + x\nF1(1) F2(2)") == "F1 ( 1 ) + 1 1 F2 ( 2 ) 2 + 2");
}

TEST_CASE("pp: macro invoked across lines and with a comment inside") {
    CHECK(pp("#define F(a, b) a + b\nF(\n  1,\n  2\n) F /* c */ ( x /* y */ , z )") == "1 + 2 x + z");
    CHECK(pp("#define F(a) <a>\nF\n(1)\nF\n\n  (2)") == "< 1 > < 2 >");
    CHECK(pp("#define F(a) <a>\nF\n+") == "F +");
}

TEST_CASE("pp: empty arguments and placeholders") {
    CHECK(pp("#define F(a, b) <a|b>\nF(,) F(1,) F(,2)") == "< | > < 1 | > < | 2 >");
    CHECK(pp("#define EMPTY\n#define F(a, b) <a|b>\nF(EMPTY, EMPTY)") == "< | >");
    CHECK(pp("#define F(a) <a>\nF() F( )") == "< > < >");
    CHECK(pp("#define F(a, b, c) a b c\nF(1,,3)") == "1 3");
}

TEST_CASE("pp: variadic macros and __VA_ARGS__ with zero arguments") {
    CHECK(pp("#define V(...) f(__VA_ARGS__)\nV() V(1) V(1, 2, 3) V((1, 2), 3)") == "f ( ) f ( 1 ) f ( 1 , 2 , 3 ) f ( ( 1 , 2 ) , 3 )");
    CHECK(pp("#define V(a, ...) g(a; __VA_ARGS__)\nV(1) V(1,) V(1, 2) V(1, 2, 3)") == "g ( 1 ; ) g ( 1 ; ) g ( 1 ; 2 ) g ( 1 ; 2 , 3 )");
    CHECK(pp("#define V(fmt, args...) p(fmt, args)\nV(\"a\", 1, 2)") == "p ( \"a\" , 1 , 2 )");
}

TEST_CASE("pp: trailing commas and the GNU comma extension") {
    // comma is dropped only when the variable argument is omitted entirely (GCC -std=c++20 behaviour)
    CHECK(pp("#define V(fmt, ...) log(fmt, ## __VA_ARGS__)\nV(\"a\") V(\"a\", 1) V(\"a\", 1, 2) V(\"a\",)") ==
          "log ( \"a\" ) log ( \"a\" , 1 ) log ( \"a\" , 1 , 2 ) log ( \"a\" , )");
    CHECK(pp("#define V(...) f(a, ## __VA_ARGS__)\nV(1)") == "f ( a , 1 )");
    CHECK(pp("#define L(x, ...) x,__VA_ARGS__,\nL(1) L(1,2)") == "1 , , 1 , 2 ,");
}

TEST_CASE("pp: __VA_OPT__") {
    CHECK(pp("#define V(...) f(0 __VA_OPT__(,) __VA_ARGS__)\nV() V(1) V(1,2)") == "f ( 0 ) f ( 0 , 1 ) f ( 0 , 1 , 2 )");
}

TEST_CASE("pp: wrong argument counts are diagnosed, never fatal") {
    Run few = preprocess({{"main.h", "#define F(a, b) a b\nF(1) tail"}});
    CHECK(few.hasDiag(Diag::Severity::Error, "arguments"));
    CHECK(few.text.find("tail") != std::string::npos);
    Run many = preprocess({{"main.h", "#define F(a) a\nF(1, 2) tail"}});
    CHECK(many.hasDiag(Diag::Severity::Error, "arguments"));
    CHECK(many.text.find("tail") != std::string::npos);
    Run unterminated = preprocess({{"main.h", "#define F(a) a\nbefore F(1, 2"}});
    CHECK(unterminated.hasDiag(Diag::Severity::Error, "unterminated"));
    CHECK(unterminated.text.find("before") != std::string::npos);
    Run zero = preprocess({{"main.h", "#define F() 1\nF(1)"}});
    CHECK(zero.hasDiag(Diag::Severity::Error));
}

// ---------------------------------------------------------------------------------------------------------------------
// # and ##

TEST_CASE("pp: stringizing") {
    CHECK(pp("#define S(x) #x\nS(a   +   b) S(  x  ) S() S(f(a,b)) S(a/**/b)") == "\"a + b\" \"x\" \"\" \"f(a,b)\" \"a b\"");
    CHECK(pp("#define S(x) #x\nS(\"q\\n\" 'c') S(\"\\\\\")") == "\"\\\"q\\\\n\\\" 'c'\" \"\\\"\\\\\\\\\\\"\"");
    CHECK(pp("#define S(x) #x\nS(a\n  b)") == "\"a b\"");
    // the argument is not expanded when stringized, but is expanded by an extra level of macro
    CHECK(pp("#define S(x) #x\n#define XS(x) S(x)\n#define A 12\nS(A) XS(A)") == "\"A\" \"12\"");
    CHECK(pp("#define S(x) #x\n#define XS(x) S(x)\nXS(__LINE__)") == "\"3\"");
    CHECK(pp("#define S(x) #x\nS(@) S(-1) S((1 , 2))") == "\"@\" \"-1\" \"(1 , 2)\"");
}

TEST_CASE("pp: token pasting produces identifiers and numbers") {
    CHECK(pp("#define CAT(a,b) a##b\nCAT(x, 1) CAT(1, x) CAT(1, 2) CAT(foo, bar)") == "x1 1x 12 foobar");
    CHECK(pp("#define CAT(a,b) a##b\nCAT(+,+) CAT(-,>) CAT(<<,=) CAT(|,|)") == "++ -> <<= ||");
    CHECK(pp("#define CAT(a,b) a##b\nCAT(a,) CAT(,b) CAT(,) done") == "a b done");
    CHECK(pp("#define CAT(a,b) a##b\nCAT(L, \"str\") CAT(L, 'c') CAT(0x, 1F) CAT(1., 5)") == "L\"str\" L'c' 0x1F 1.5");
    CHECK(pp("#define t(x,y,z) x ## y ## z\nint j[] = { t(1,2,3), t(,4,5), t(6,,7), t(8,9,), t(10,,), t(,11,), t(,,12), t(,,) };") ==
          "int j [ ] = { 123 , 45 , 67 , 89 , 10 , 11 , 12 , } ;");
    // pasted identifiers are macro-expanded afterwards
    CHECK(pp("#define CAT(a,b) a##b\n#define AB 42\nCAT(A,B)") == "42");
    // arguments next to ## are not pre-expanded, through an extra level they are
    CHECK(pp("#define CAT(a,b) a##b\n#define XCAT(a,b) CAT(a,b)\n#define NAME X\nCAT(NAME,1) XCAT(NAME,1)") == "NAME1 X1");
    CHECK(pp("#define OBJ x ## y\nOBJ") == "xy");
    CHECK(pp("#define mk(n) var_##n##_end\nmk(3) mk(abc)") == "var_3_end var_abc_end");
    CHECK(pp("#define hash_hash # ## #\n#define mkstr(a) # a\n#define in_between(a) mkstr(a)\n#define join(c, d) in_between(c hash_hash d)\njoin(x, y)") ==
          "\"x ## y\"");
}

TEST_CASE("pp: invalid paste is diagnosed and keeps both tokens") {
    Run r = preprocess({{"main.h", "#define CAT(a,b) a##b\nCAT(+, -) next"}});
    CHECK(r.hasDiag(Diag::Severity::Warning, "pasting"));
    CHECK(r.text == "+ - next");
    Run end = preprocess({{"main.h", "#define BAD ## x\nBAD"}});
    CHECK(end.hasDiag(Diag::Severity::Error, "##"));
}

TEST_CASE("pp: C standard examples") {
    const std::string code =
        "#define x 3\n#define f(a) f(x * (a))\n#undef x\n#define x 2\n#define g f\n#define z z[0]\n#define h g(~\n#define m(a) a(w)\n#define w 0,1\n"
        "#define t(a) a\n#define p() int\n#define q(x) x\n#define r(x,y) x ## y\n#define str(x) # x\n"
        "f(y+1) + f(f(z)) % t(t(g)(0) + t)(1);\n"
        "g(x+(3,4)-w) | h 5) & m\n(f)^m(m);\n"
        "p() i[q()] = { q(1), r(2,3), r(4,), r(,5), r(,) };\n"
        "char c[2][6] = { str(hello), str() };\n";
    CHECK(pp(code) ==
          "f ( 2 * ( y + 1 ) ) + f ( 2 * ( f ( 2 * ( z [ 0 ] ) ) ) ) % f ( 2 * ( 0 ) ) + t ( 1 ) ; "
          "f ( 2 * ( 2 + ( 3 , 4 ) - 0 , 1 ) ) | f ( 2 * ( ~ 5 ) ) & f ( 2 * ( 0 , 1 ) ) ^ m ( 0 , 1 ) ; "
          "int i [ ] = { 1 , 23 , 4 , 5 , } ; "
          "char c [ 2 ] [ 6 ] = { \"hello\" , \"\" } ;");
}

// ---------------------------------------------------------------------------------------------------------------------
// predefined / special macros

TEST_CASE("pp: __LINE__, __FILE__, __COUNTER__") {
    Run r = preprocess({{"main.h", "a __LINE__\n__FILE__\n#include \"sub/x.h\"\n__COUNTER__ __COUNTER__\n"}, {"sub/x.h", "\n__FILE__ __LINE__ __INCLUDE_LEVEL__\n"}});
    CHECK(r.text == "a 1 \"main.h\" \"sub/x.h\" 2 1 0 1");
    CHECK(pp("#define L __LINE__\n\n\nL") == "4");
}

TEST_CASE("pp: _Pragma operator disappears") {
    CHECK(pp("a _Pragma(\"once\") b") == "a b");
    CHECK(pp("#define P(x) _Pragma(#x) done\na P(pack(1)) b") == "a done b");
}

// ---------------------------------------------------------------------------------------------------------------------
// conditionals

TEST_CASE("pp: #if / #elif / #else / #endif selection and nesting") {
    CHECK(pp("#if 1\na\n#else\nb\n#endif") == "a");
    CHECK(pp("#if 0\na\n#else\nb\n#endif") == "b");
    CHECK(pp("#if 0\na\n#elif 0\nb\n#elif 1\nc\n#elif 1\nd\n#else\ne\n#endif") == "c");
    CHECK(pp("#if 0\na\n#elif 0\nb\n#else\ne\n#endif") == "e");
    CHECK(pp("#if 0\n#if 1\na\n#else\nb\n#endif\n#elif 1\nc\n#endif") == "c");
    CHECK(pp("#if 1\n#if 0\na\n#elif 1\nb\n#endif\n#else\nc\n#endif") == "b");
    CHECK(pp("#if 0\n#define SKIPPED 1\n#error nope\n#include \"missing.h\"\n#endif\nSKIPPED") == "SKIPPED");
    CHECK(pp("#ifdef A\na\n#endif\n#ifndef A\nb\n#endif\n#define A\n#ifdef A\nc\n#endif\n#ifndef A\nd\n#endif") == "b c");
    CHECK(pp("#  if 1\n  #  define V 3\n#   endif\nV") == "3");
}

TEST_CASE("pp: #if expressions: integers, ternary, shifts, logic") {
    CHECK(pp("#if 1 ? 2 : 0\nyes\n#endif") == "yes");
    CHECK(pp("#if 1 ? 0 : 1\nyes\n#else\nno\n#endif") == "no");
    CHECK(pp("#if 0 ? 1 : 2 ? 3 : 4\nyes\n#endif") == "yes");
    CHECK(pp("#if (1 ? 2 : 0) == 2 && !(0 ? 1 : 0)\nyes\n#endif") == "yes");
    CHECK(pp("#if 1 << 3 == 8 && (1 << 62) > 0 && (-1 >> 1) == -1 && (1ULL << 63) > 0 && (256 >> 4) == 16\nyes\n#endif") == "yes");
    CHECK(pp("#if (1 << 63) < 0\nyes\n#endif") == "yes"); // wraps like GCC
    CHECK(pp("#if 2 - 1 - 1 == 0 && 8 / 2 / 2 == 2 && 2 + 3 * 4 == 14 && (2 + 3) * 4 == 20\nyes\n#endif") == "yes");
    CHECK(pp("#if 10 / 3 == 3 && 10 % 3 == 1 && -7 / 2 == -3 && -7 % 2 == -1\nyes\n#endif") == "yes");
    CHECK(pp("#if ~0 == -1 && !0 == 1 && !5 == 0 && -(-1) == 1 && +1 == 1\nyes\n#endif") == "yes");
    CHECK(pp("#if (3 & 6) == 2 && (3 | 4) == 7 && (3 ^ 1) == 2 && (2 || 3) == 1 && (0 && 3) == 0\nyes\n#endif") == "yes");
    CHECK(pp("#if 1 < 2 && 2 > 1 && 2 <= 2 && 2 >= 2 && 1 != 2 && 3 == 3\nyes\n#endif") == "yes");
    CHECK(pp("#if 3 > 2 > 1\nyes\n#else\nno\n#endif") == "no");
    CHECK(pp("#if 1 ? 2 ? 3 : 4 : 5\nyes\n#endif") == "yes");
    CHECK(pp("#if (1, 2) == 2\nyes\n#endif") == "yes");
}

TEST_CASE("pp: #if numeric literals, suffixes, signedness and character constants") {
    CHECK(pp("#if 1ULL && 2u && 3L && 4LL && 5ul && 6llu && 7i64 && 8ui64\nyes\n#endif") == "yes");
    CHECK(pp("#if 0x10 == 16 && 010 == 8 && 0b101 == 5 && 1'000 == 1000 && 0xFFULL == 255 && 0 == 00\nyes\n#endif") == "yes");
    CHECK(pp("#if 0xFFFFFFFFFFFFFFFF == -1\nyes\n#endif") == "yes");
    CHECK(pp("#if -1 < 0u\nno\n#else\nyes\n#endif") == "yes"); // -1 converts to unsigned
    CHECK(pp("#if -1 < 0\nyes\n#endif") == "yes");
    CHECK(pp("#if 18446744073709551615u > 0 && 9223372036854775807 > 0\nyes\n#endif") == "yes");
    CHECK(pp("#if 'a' == 97 && '\\n' == 10 && '\\x41' == 65 && '\\0' == 0 && '\\377' < 0 && L'\\377' > 0 && '\\101' == 65\nyes\n#endif") == "yes");
    CHECK(pp("#if 'ab' == 24930\nyes\n#endif") == "yes");
    CHECK(pp("#if 1000000000000 * 1000 > 0\nyes\n#endif") == "yes");
}

TEST_CASE("pp: #if identifiers, defined, undefined names and macro expansion") {
    CHECK(pp("#if UNDEF == 0 && !UNDEF\nyes\n#endif") == "yes");
    CHECK(pp("#define N 5\n#define EMPTY\n#if defined N && defined(N) && !defined(UNDEF) && defined ( EMPTY ) && !defined UNDEF\nyes\n#endif") == "yes");
    CHECK(pp("#define N 5\n#if N > 4 && N >= 5 && N == 5\nyes\n#endif") == "yes");
    CHECK(pp("#define IS_ZERO(x) (x == 0)\n#if IS_ZERO(0) && !IS_ZERO(1)\nyes\n#endif") == "yes");
    CHECK(pp("#define D defined(X)\n#define X\n#if D\nyes\n#endif") == "yes");
    CHECK(pp("#if true && !false\nyes\n#endif") == "yes");
    CHECK(pp("#define A B\n#define B 0\n#if A\nno\n#else\nyes\n#endif") == "yes");
    CHECK(pp("#define F(x) x\n#if F(0) || F(1)\nyes\n#endif") == "yes");
    CHECK(pp("#if defined(__has_include) && __has_include(<x>) == 0\nyes\n#endif") == "yes");
    CHECK(pp("#if __has_attribute(noreturn) == 0\nyes\n#endif") == "yes");
}

TEST_CASE("pp: #if evaluation is lazy for division by zero") {
    Run lazy = preprocess({{"main.h", "#if 0 && (1 / 0)\n#elif 1 || (1 / 0)\nyes\n#endif\n#if 1 ? 2 : (1 % 0)\nyes2\n#endif"}});
    CHECK(lazy.text == "yes yes2");
    CHECK(lazy.result.diags.empty());
    Run hard = preprocess({{"main.h", "#if 1 / 0\nno\n#else\nyes\n#endif"}});
    CHECK(hard.hasDiag(Diag::Severity::Error, "division"));
    CHECK(hard.text == "yes");
}

TEST_CASE("pp: malformed #if expressions are diagnosed and evaluate to false") {
    for (const char* expr : {"", "(", "1 +", "1 2", ")", "1 ? 2", "\"str\"", "1.5", "0x", "09", "1 +* 2", "defined", "defined(", "defined(1)", "@"}) {
        CAPTURE(expr);
        Run r = preprocess({{"main.h", std::string("#if ") + expr + "\nyes\n#else\nno\n#endif\nafter"}});
        CHECK(r.hasDiag(Diag::Severity::Error));
        CHECK(r.text == "no after");
    }
}

TEST_CASE("pp: deeply nested expression does not overflow the stack") {
    const std::string deep = std::string(100000, '(') + "1" + std::string(100000, ')');
    Run r = preprocess({{"main.h", "#if " + deep + "\nyes\n#else\nno\n#endif\nafter"}});
    CHECK(r.hasDiag(Diag::Severity::Error, "deeply"));
    CHECK(r.text == "no after");
    Run bangs = preprocess({{"main.h", "#if " + std::string(100000, '!') + "1\nyes\n#endif\nafter"}});
    CHECK(bangs.hasDiag(Diag::Severity::Error));
}

TEST_CASE("pp: conditional structure errors") {
    CHECK(preprocess({{"main.h", "#endif\nx"}}).hasDiag(Diag::Severity::Error, "#endif without"));
    CHECK(preprocess({{"main.h", "#else\nx"}}).hasDiag(Diag::Severity::Error, "#else without"));
    CHECK(preprocess({{"main.h", "#elif 1\nx"}}).hasDiag(Diag::Severity::Error, "#elif without"));
    Run open = preprocess({{"main.h", "#if 1\nx"}});
    CHECK(open.hasDiag(Diag::Severity::Error, "unterminated"));
    CHECK(open.text == "x");
    CHECK(preprocess({{"main.h", "#if 1\n#else\n#else\n#endif"}}).hasDiag(Diag::Severity::Error, "#else after #else"));
    CHECK(preprocess({{"main.h", "#ifdef\n#endif"}}).hasDiag(Diag::Severity::Error));
    // an #if opened in an included file must not leak into the includer
    Run leak = preprocess({{"main.h", "#include \"a.h\"\nvisible"}, {"a.h", "#if 0\nhidden"}});
    CHECK(leak.text == "visible");
    CHECK(leak.hasDiag(Diag::Severity::Error, "unterminated"));
}

// ---------------------------------------------------------------------------------------------------------------------
// #include

TEST_CASE("pp: quoted include searches the including directory first, then includeDirs") {
    Run r = preprocess({{"main.h", "#include \"sub/a.h\"\n#include \"b.h\""},
                        {"sub/a.h", "A\n#include \"b.h\"\n#include \"../c.h\""},
                        {"sub/b.h", "subB"},
                        {"b.h", "rootB"},
                        {"c.h", "C"}},
                       "main.h");
    CHECK(r.text == "A subB C rootB");
    CHECK(r.result.files == std::vector<std::string>{"main.h", "sub/a.h", "sub/b.h", "c.h", "b.h"});
}

TEST_CASE("pp: include directories are searched in order; angle includes use only includeDirs") {
    std::map<std::string, std::string> files{{"main.h", "#include <x.h>\n#include \"y.h\""},
                                             {"inc1/x.h", "x1"},
                                             {"inc2/x.h", "x2"},
                                             {"inc2/y.h", "y2"},
                                             {"x.h", "xroot"},
                                             {"y.h", "yroot"}};
    CHECK(preprocess(files, "main.h", {}, {"inc1", "inc2"}).text == "x1 yroot"); // quoted: own directory (root) first
    CHECK(preprocess(files, "main.h", {}, {"inc2", "inc1"}).text == "x2 yroot");
    CHECK(preprocess(files, "main.h", {}, {""}).text == "xroot yroot");
    files["sub/main.h"] = "#include \"y.h\" #include <y.h>";
    files["sub/main.h"] = "#include \"y.h\"\n#include <y.h>";
    CHECK(preprocess(files, "sub/main.h", {}, {"inc2"}).text == "y2 y2");
}

TEST_CASE("pp: paths with .. are normalized, the same file is identified once") {
    Run r = preprocess({{"main.h", "#include \"a/b/../c.h\"\n#include \"a/c.h\"\n#include \"./a/./c.h\""}, {"a/c.h", "#pragma once\nC"}});
    CHECK(r.text == "C");
    CHECK(r.result.files.size() == 2);
}

TEST_CASE("pp: unresolved includes") {
    // silent system headers
    Run quiet = preprocess({{"main.h", "#include <immintrin.h>\n#include <vector>\nx"}});
    CHECK(quiet.text == "x");
    CHECK(quiet.result.diags.empty());
    // loud
    MapSource src({{"main.h", "#include <immintrin.h>\nx"}});
    PreprocessOptions opt;
    opt.includeDirs = {""};
    opt.silentSystemIncludes = false;
    PreprocessResult loud = Preprocessor(src, opt).run("main.h");
    REQUIRE(loud.diags.size() == 1);
    CHECK(loud.diags[0].severity == Diag::Severity::Warning);
    CHECK(loud.diags[0].file == "main.h");
    CHECK(loud.diags[0].line == 1);
    // a missing quoted include is always a warning
    Run missing = preprocess({{"main.h", "#include \"missing.h\"\nx"}});
    CHECK(missing.hasDiag(Diag::Severity::Warning, "missing.h"));
    CHECK(missing.text == "x");
    // malformed
    CHECK(preprocess({{"main.h", "#include\nx"}}).hasDiag(Diag::Severity::Error));
    CHECK(preprocess({{"main.h", "#include <unterminated\nx"}}).hasDiag(Diag::Severity::Error));
    CHECK(preprocess({{"main.h", "#include 42\nx"}}).hasDiag(Diag::Severity::Error));
}

TEST_CASE("pp: computed includes") {
    CHECK(preprocess({{"main.h", "#define HDR \"a.h\"\n#include HDR"}, {"a.h", "A"}}).text == "A");
    CHECK(preprocess({{"main.h", "#define HDR <inc/a.h>\n#include HDR"}, {"inc/a.h", "A"}}).text == "A");
    CHECK(preprocess({{"main.h", "#define S(x) #x\n#define Q(x) S(x.h)\n#include Q(b)"}, {"b.h", "B"}}).text == "B");
    CHECK(preprocess({{"main.h", "#include <inc/a.h>"}, {"inc/a.h", "A"}}).text == "A");
}

TEST_CASE("pp: #pragma once") {
    Run r = preprocess({{"main.h", "#include \"a.h\"\n#include \"a.h\"\n#include \"b.h\"\n#include \"a.h\""},
                        {"a.h", "#pragma once\nA\n"},
                        {"b.h", "#include \"a.h\"\nB"}});
    CHECK(r.text == "A B");
}

TEST_CASE("pp: classic include guards") {
    std::map<std::string, std::string> files{{"main.h", "#include \"g.h\"\n#include \"g.h\"\n#include \"h.h\"\n#include \"h.h\"\n#include \"i.h\"\n#include \"i.h\""},
                                             {"g.h", "#ifndef G_H\n#define G_H\ng\n#endif\n"},
                                             {"h.h", "// comment\n#if !defined(H_H)\n#define H_H 1\nh\n#endif // end\n"},
                                             {"i.h", "#ifndef I_H\ni\n#endif\n"}}; // not a real guard: I_H never defined
    Run r = preprocess(files);
    CHECK(r.text == "g h i i");
    // content after the closing #endif disables the guard shortcut but the semantics stay correct
    Run tail = preprocess({{"main.h", "#include \"g.h\"\n#include \"g.h\""}, {"g.h", "#ifndef G\n#define G\na\n#endif\nb\n"}});
    CHECK(tail.text == "a b b");
    // undefining the guard makes the file visible again
    Run again = preprocess({{"main.h", "#include \"g.h\"\n#undef G_H\n#include \"g.h\""}, {"g.h", "#ifndef G_H\n#define G_H\ng\n#endif\n"}});
    CHECK(again.text == "g g");
}

TEST_CASE("pp: circular includes terminate") {
    // with guards
    Run guarded = preprocess({{"main.h", "#include \"a.h\""},
                              {"a.h", "#ifndef A\n#define A\na1\n#include \"b.h\"\na2\n#endif"},
                              {"b.h", "#ifndef B\n#define B\nb1\n#include \"a.h\"\nb2\n#endif"}});
    CHECK(guarded.text == "a1 b1 b2 a2");
    CHECK(guarded.result.diags.empty());
    // with #pragma once
    Run once = preprocess({{"main.h", "#include \"a.h\""},
                           {"a.h", "#pragma once\na1\n#include \"b.h\"\na2"},
                           {"b.h", "#pragma once\nb1\n#include \"a.h\"\nb2"}});
    CHECK(once.text == "a1 b1 b2 a2");
    // without any protection the depth limit stops the recursion
    MapSource src({{"main.h", "#include \"main.h\"\nx"}});
    PreprocessOptions opt;
    opt.includeDirs = {""};
    opt.maxIncludeDepth = 20;
    PreprocessResult r = Preprocessor(src, opt).run("main.h");
    bool depthError = false;
    for (const Diag& d : r.diags) depthError |= d.severity == Diag::Severity::Error && d.message.find("nested too deeply") != std::string::npos;
    CHECK(depthError);
    CHECK(r.tokens.size() == 1 + 20); // 20 levels each contribute one x
    // self include with a macro-dependent counter terminates through #if
    Run counter = preprocess({{"main.h", "#define N 0\n#include \"c.h\""}, {"c.h", "n\n#if N < 3\n#undef N\n#define N 3\n#include \"c.h\"\n#endif"}});
    CHECK(counter.text == "n n");
}

TEST_CASE("pp: include of a file spanning a macro call is not merged") {
    // tokens of different files stay separate: a macro call cannot silently continue into the next file
    Run r = preprocess({{"main.h", "#define F(a) <a>\n#include \"x.h\"\nafter"}, {"x.h", "F(1)\n"}});
    CHECK(r.text == "< 1 > after");
}

TEST_CASE("pp: __has_include") {
    CHECK(preprocess({{"main.h", "#if __has_include(<inc/a.h>)\nyes\n#else\nno\n#endif"}, {"inc/a.h", ""}}).text == "yes");
    CHECK(preprocess({{"main.h", "#if __has_include(<nothere.h>)\nyes\n#else\nno\n#endif"}}).text == "no");
    CHECK(preprocess({{"main.h", "#if __has_include(\"a.h\") && !__has_include(\"b.h\")\nyes\n#endif"}, {"a.h", ""}}).text == "yes");
    CHECK(preprocess({{"main.h", "#ifdef __has_include\nyes\n#endif"}}).text == "yes");
    CHECK(preprocess({{"main.h", "#if defined(__has_include)\n#if __has_include(<cstdint>)\n#include <cstdint>\n#elif __has_include(<stdint.h>)\n#include <stdint.h>\n#else\nfallback\n#endif\n#endif"},
                      {"stdint.h", "stdint"}})
              .text == "stdint");
}

// ---------------------------------------------------------------------------------------------------------------------
// other directives

TEST_CASE("pp: #error and #warning become diagnostics with file and line") {
    Run r = preprocess({{"main.h", "a\n#error Something bad: 'quoted'\nb\n#warning careful\nc"}});
    CHECK(r.text == "a b c");
    REQUIRE(r.result.diags.size() == 2);
    CHECK(r.result.diags[0].severity == Diag::Severity::Error);
    CHECK(r.result.diags[0].line == 2);
    CHECK(r.result.diags[0].file == "main.h");
    CHECK(r.result.diags[0].message.find("Something bad") != std::string::npos);
    CHECK(r.result.diags[1].severity == Diag::Severity::Warning);
    CHECK(r.result.diags[1].line == 4);
}

TEST_CASE("pp: #line, #pragma, null and unknown directives are harmless") {
    CHECK(pp("a\n#line 100 \"foo.c\"\nb\n# 5 \"x.c\"\nc\n#\nd\n#ident \"v\"\ne") == "a b c d e");
    CHECK(pp("#pragma warning(push)\n#pragma GCC diagnostic ignored \"-Wall\"\nx") == "x");
    Run unknown = preprocess({{"main.h", "#frobnicate 1\nx"}});
    CHECK(unknown.text == "x");
    CHECK(unknown.hasDiag(Diag::Severity::Warning, "frobnicate"));
    CHECK(pp("#define\n#undef\nx").find('x') != std::string::npos);
}

TEST_CASE("pp: pragmas are recorded with their token position") {
    Run r = preprocess({{"main.h", "a\n#pragma pack(push, 1)\nb c\n#pragma pack(pop)\n#pragma once\nd"}});
    CHECK(r.text == "a b c d");
    REQUIRE(r.result.pragmas.size() == 2);
    CHECK(r.result.pragmas[0].text == "pack(push, 1)");
    CHECK(r.result.pragmas[0].tokenIndex == 1);
    CHECK(r.result.pragmas[0].line == 2);
    CHECK(r.result.pragmas[1].text == "pack(pop)");
    CHECK(r.result.pragmas[1].tokenIndex == 3);
}

TEST_CASE("pp: #pragma push_macro / pop_macro") {
    CHECK(pp("#define A 1\n#pragma push_macro(\"A\")\n#undef A\n#define A 2\nA\n#pragma pop_macro(\"A\")\nA") == "2 1");
}

TEST_CASE("pp: line continuation and comments in directives") {
    CHECK(pp("#define SUM(a, b) \\\n    ((a) + \\\n     (b)) // trailing\nSUM(1,\n2)") == "( ( 1 ) + ( 2 ) )");
    CHECK(pp("#define A 1 /* multi\nline */ + 2\nA") == "1 + 2");
    CHECK(pp("#if 1 /* c */ // d\nyes\n#endif") == "yes");
}

TEST_CASE("pp: finalMacros renders the macro table") {
    Run r = preprocess({{"main.h", "#define A 1 + 2\n#define F(x, y) x ## y # x\n#define V(a, ...) a __VA_ARGS__\n#define E\n#define U 1\n#undef U"}});
    std::map<std::string, std::string> m(r.result.finalMacros.begin(), r.result.finalMacros.end());
    CHECK(m["A"] == "A 1 + 2");
    CHECK(m["F"] == "F(x,y) x ## y #x");
    CHECK(m["V"] == "V(a,...) a __VA_ARGS__");
    CHECK(m["E"] == "E");
    CHECK(m.count("U") == 0);
    CHECK(m.count("__LINE__") == 0);
}

TEST_CASE("pp: UTF-8 BOM and CRLF line endings") {
    Run r = preprocess({{"main.h", "\xEF\xBB\xBF#define A 1\r\nA\r\n#if A\r\nyes\r\n#endif\r\n"}});
    CHECK(r.text == "1 yes");
    CHECK(r.result.diags.empty());
}

// ---------------------------------------------------------------------------------------------------------------------
// robustness

TEST_CASE("pp: malformed definitions are diagnosed") {
    CHECK(preprocess({{"main.h", "#define 1 2\nx"}}).hasDiag(Diag::Severity::Error));
    CHECK(preprocess({{"main.h", "#define F(a, a) 1\nx"}}).hasDiag(Diag::Severity::Error, "duplicate"));
    CHECK(preprocess({{"main.h", "#define F(a 1\nx"}}).hasDiag(Diag::Severity::Error));
    CHECK(preprocess({{"main.h", "#define F(a,) 1\nx"}}).hasDiag(Diag::Severity::Error));
    CHECK(preprocess({{"main.h", "#define F(...,a) 1\nx"}}).hasDiag(Diag::Severity::Error));
    CHECK(preprocess({{"main.h", "#define A ##\nx"}}).hasDiag(Diag::Severity::Error));
    Run r = preprocess({{"main.h", "#define F(a, a) 1\nF(1,2) x"}});
    CHECK(r.text == "F ( 1 , 2 ) x"); // the broken macro is simply not defined
}

TEST_CASE("pp: garbage input never crashes") {
    const char* inputs[] = {"#", "#define", "#define A(", "#include", "#if", "#else", "#endif", "###", "# # #", "\"unterminated", "'", "/* never closed",
                            "#define F(x) #", "#define F(x) x ##", "F(", "#define F(x) F(x\nF(1", "\\", "#if (\n", "#define X X X X\nX", "#include \"", "#include <"};
    for (const char* in : inputs) {
        CAPTURE(in);
        Run r = preprocess({{"main.h", in}});
        REQUIRE(!r.result.tokens.empty());
        CHECK(r.result.tokens.back().kind == TokKind::End);
    }
    // binary noise
    std::string noise;
    unsigned seed = 12345;
    for (int i = 0; i < 20000; ++i) {
        seed = seed * 1103515245u + 12345u;
        noise.push_back("#()\\,.\n \"'abc##%*/#defineif"[(seed >> 16) % 28]);
    }
    Run n = preprocess({{"main.h", noise}});
    CHECK(n.result.tokens.back().kind == TokKind::End);
}

TEST_CASE("pp: self-referential recursion through function-like macros terminates") {
    CHECK(pp("#define f(x) g(x)\n#define g(x) f(x)\nf(1)") == "f ( 1 )");
    CHECK(pp("#define EXP(x) EXP(x) x\nEXP(EXP(1))") == "EXP ( EXP ( 1 ) 1 ) EXP ( 1 ) 1");
}

TEST_CASE("pp: expansion depth limit") {
    std::string code;
    for (int i = 0; i < 300; ++i) code += "#define M" + std::to_string(i) + " M" + std::to_string(i + 1) + " +\n";
    code += "#define M300 end\nM0\n";
    MapSource src({{"main.h", code}});
    PreprocessOptions opt;
    opt.includeDirs = {""};
    opt.maxExpansionDepth = 64;
    PreprocessResult r = Preprocessor(src, opt).run("main.h");
    CHECK(r.tokens.back().kind == TokKind::End);
    bool reported = false;
    for (const Diag& d : r.diags) reported |= d.severity == Diag::Severity::Error && d.message.find("depth") != std::string::npos;
    CHECK(reported);
    // with the default limit a chain of 200 is fine
    std::string ok;
    for (int i = 0; i < 200; ++i) ok += "#define N" + std::to_string(i) + " N" + std::to_string(i + 1) + "\n";
    ok += "#define N200 end\nN0\n";
    CHECK(preprocess({{"main.h", ok}}).text == "end");
}

TEST_CASE("pp: exponential expansion is stopped by the output budget") {
    std::string code = "#define E0 x\n";
    for (int i = 1; i <= 40; ++i) code += "#define E" + std::to_string(i) + " E" + std::to_string(i - 1) + " E" + std::to_string(i - 1) + "\n";
    code += "E40\nafter\n";
    MapSource src({{"main.h", code}});
    PreprocessOptions opt;
    opt.includeDirs = {""};
    opt.maxOutputTokens = 100000;
    PreprocessResult r = Preprocessor(src, opt).run("main.h");
    CHECK(r.aborted);
    CHECK(r.tokens.size() <= opt.maxOutputTokens + 1);
    CHECK(r.tokens.back().kind == TokKind::End);
    bool reported = false;
    for (const Diag& d : r.diags) reported |= d.severity == Diag::Severity::Error;
    CHECK(reported);

    // exponential growth through arguments
    std::string args = "#define D(x) x x\n";
    std::string call = "x";
    for (int i = 0; i < 40; ++i) call = "D(" + call + ")";
    args += call + "\n";
    MapSource src2({{"main.h", args}});
    PreprocessResult r2 = Preprocessor(src2, opt).run("main.h");
    CHECK(r2.aborted);
    CHECK(r2.tokens.back().kind == TokKind::End);
}

TEST_CASE("pp: large synthetic input is fast") {
    std::string code = "#define MUL(a, b) ((a) * (b))\n#define CAT(a, b) a##b\n#define ID(x) x\n";
    for (int i = 0; i < 20000; ++i)
        code += "int CAT(v, " + std::to_string(i) + ") = ID(MUL(" + std::to_string(i) + ", ID(3))); // line\n";
    const auto t0 = std::chrono::steady_clock::now();
    Run r = preprocess({{"main.h", code}});
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.result.diags.empty());
    CHECK(r.result.tokens.size() > 20000 * 12);
    CHECK(ms < 5000);
}

// ---------------------------------------------------------------------------------------------------------------------
// real data

namespace {

namespace fs = std::filesystem;

const std::vector<std::pair<std::string, std::string>> kLinuxMsvcLikeDefines = {{"_MSC_VER", "1944"}, {"_WIN64", "1"}, {"_M_X64", "1"}};

PreprocessResult preprocessCore(const std::string& core, const std::vector<std::pair<std::string, std::string>>& defines, bool silent = true) {
    DiskSource src(core);
    PreprocessOptions opt;
    opt.includeDirs = {"src", ""};
    opt.defines = defines;
    opt.silentSystemIncludes = silent;
    return Preprocessor(src, opt).run("src/contract_core/contract_def.h");
}

#if defined(__unix__) || defined(__APPLE__)
bool gccAvailable() { return std::system("g++ --version > /dev/null 2>&1") == 0; }

// Runs `g++ -E -P` on the root file with the given defines and returns its tokens (pragma lines removed,
// absolute paths in __FILE__ strings made relative to the core directory).
std::optional<std::vector<std::string>> gccTokens(const std::string& core, const std::vector<std::pair<std::string, std::string>>& defines) {
    const fs::path dir = fs::temp_directory_path() / ("qstate_pp_oracle_" + std::to_string(std::hash<std::string>{}(core)));
    fs::remove_all(dir);
    fs::create_directories(dir / "stubs");
    for (const char* h : {"intrin.h", "immintrin.h", "stddef.h", "cstdio", "cstdlib", "cstdbool"}) std::ofstream(dir / "stubs" / h) << "\n";
    std::string cmd = "g++ -E -P -x c++ -std=c++20 -undef -nostdinc -nostdinc++ -isystem '" + (dir / "stubs").string() + "'";
    for (const auto& [k, v] : defines) cmd += " '-D" + k + "=" + v + "'";
    cmd += " -I '" + core + "/src' -I '" + core + "' '" + core + "/src/contract_core/contract_def.h' -o '" + (dir / "out.ii").string() + "' 2> /dev/null";
    if (std::system(cmd.c_str()) != 0) return std::nullopt;
    std::ifstream in(dir / "out.ii", std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::istringstream lines(ss.str());
    std::string line, filtered;
    while (std::getline(lines, line)) {
        const std::size_t p = line.find_first_not_of(" \t");
        if (p != std::string::npos && line[p] == '#') continue; // #pragma lines
        filtered += line;
        filtered += '\n';
    }
    fs::remove_all(dir);
    std::vector<std::string> out;
    for (const Token& t : lex(filtered)) {
        if (t.kind == TokKind::End) break;
        std::string text = t.text;
        const std::size_t p = text.find(core + "/");
        if (t.kind == TokKind::StringLit && p != std::string::npos) text.erase(p, core.size() + 1);
        out.push_back(std::move(text));
    }
    return out;
}
#endif

} // namespace

TEST_CASE("pp real data: contract_def.h preprocesses cleanly with the msvc-like defines") {
    const std::string core = qstate::testing::coreRepo();
    if (core.empty()) {
        MESSAGE("QSTATE_TEST_CORE_REPO not set, skipping");
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    PreprocessResult r = preprocessCore(core, kLinuxMsvcLikeDefines, /*silent=*/false);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("preprocessed " << r.tokens.size() << " tokens from " << r.files.size() << " files in " << ms << " ms");

    CHECK(!r.aborted);
    CHECK(r.tokens.size() > 100000);
    CHECK(r.files.size() > 30);
    CHECK(r.tokens.back().kind == TokKind::End);
    // Only the two MSVC / libc system headers are unresolved; nothing else (no #error, no missing quoted include).
    for (const Diag& d : r.diags) {
        CAPTURE(d.file);
        CAPTURE(d.line);
        CAPTURE(d.message);
        CHECK(d.severity == Diag::Severity::Warning);
        CHECK(d.message.find("cannot find include file <") == 0);
    }
    CHECK(r.diags.size() <= 2);
    // the contract table is visible
    bool sawContractDescriptions = false;
    for (const Token& t : r.tokens) sawContractDescriptions |= t.kind == TokKind::Ident && t.text == "contractDescriptions";
    CHECK(sawContractDescriptions);
    // generous bound for slow debug builds; RelWithDebInfo is ~0.15 s
    CHECK(ms < 3000);
    // the tokens' files are valid indexes
    for (const Token& t : r.tokens) {
        if (t.file >= r.files.size()) {
            CHECK(t.file < r.files.size());
            break;
        }
    }
}

TEST_CASE("pp real data: token stream equals g++ -E for several define sets") {
    const std::string core = qstate::testing::coreRepo();
    if (core.empty()) {
        MESSAGE("QSTATE_TEST_CORE_REPO not set, skipping");
        return;
    }
#if defined(__unix__) || defined(__APPLE__)
    if (!gccAvailable()) {
        MESSAGE("g++ not available, skipping oracle comparison");
        return;
    }
    using Defines = std::vector<std::pair<std::string, std::string>>;
    std::vector<Defines> sets = {
        kLinuxMsvcLikeDefines,
        {},
        {{"_MSC_VER", "1944"}, {"_WIN64", "1"}, {"_M_X64", "1"}, {"SINGLE_COMPILE_UNIT", "1"}},
        {{"_MSC_VER", "1944"}, {"_WIN64", "1"}, {"_M_X64", "1"}, {"NO_UEFI", "1"}, {"INCLUDE_CONTRACT_TEST_EXAMPLES", "1"}},
    };
    for (const Defines& defines : sets) {
        std::string label;
        for (const auto& d : defines) label += d.first + " ";
        CAPTURE(label);
        auto expected = gccTokens(core, defines);
        REQUIRE(expected.has_value());
        PreprocessResult r = preprocessCore(core, defines);
        REQUIRE(!r.tokens.empty());
        const std::size_t mine = r.tokens.size() - 1; // without End
        std::size_t i = 0;
        while (i < mine && i < expected->size() && r.tokens[i].text == (*expected)[i]) ++i;
        if (i != mine || i != expected->size()) {
            std::string context;
            for (std::size_t k = i > 8 ? i - 8 : 0; k < std::min(mine, i + 8); ++k) context += r.tokens[k].text + " ";
            std::string gcc;
            for (std::size_t k = i > 8 ? i - 8 : 0; k < std::min(expected->size(), i + 8); ++k) gcc += (*expected)[k] + " ";
            INFO("first divergence at token " << i << "\n mine: " << context << "\n gcc : " << gcc << "\n at " << (i < mine ? r.files[r.tokens[i].file] + ":" + std::to_string(r.tokens[i].line) : std::string("end")));
            CHECK(false);
        } else {
            CHECK(mine == expected->size());
        }
    }
#else
    MESSAGE("oracle comparison needs a POSIX shell, skipping");
#endif
}
