// Constant evaluator: operators, integer types, literals, casts, sizeof, names, templates in expressions.
#include <doctest/doctest.h>

#include "program_test_util.h"

using namespace qstate::cpp;
using namespace qstate::cpp::testutil;

namespace {

std::optional<ConstValue> eval(Program& p, const std::string& e, const std::string& scope = {}, std::string* err = nullptr) {
    return p.evalConstant(e, scope, err);
}

std::int64_t val(Program& p, const std::string& e, const std::string& scope = {}) {
    std::string err;
    auto v = eval(p, e, scope, &err);
    INFO("expression: " << e << " error: " << err);
    REQUIRE(v.has_value());
    return v->value;
}

} // namespace

TEST_CASE("consteval: all C operators and precedence") {
    auto prog = parseText("");
    Program& p = *prog;
    CHECK(val(p, "1 + 2 * 3") == 7);
    CHECK(val(p, "(1 + 2) * 3") == 9);
    CHECK(val(p, "10 - 3 - 2") == 5);
    CHECK(val(p, "100 / 7") == 14);
    CHECK(val(p, "100 % 7") == 2);
    CHECK(val(p, "-7 / 2") == -3);
    CHECK(val(p, "-7 % 3") == -1);
    CHECK(val(p, "1 << 4") == 16);
    CHECK(val(p, "256 >> 4") == 16);
    CHECK(val(p, "-16 >> 2") == -4);
    CHECK(val(p, "6 & 3") == 2);
    CHECK(val(p, "6 | 3") == 7);
    CHECK(val(p, "6 ^ 3") == 5);
    CHECK(val(p, "~0") == -1);
    CHECK(val(p, "!0") == 1);
    CHECK(val(p, "!5") == 0);
    CHECK(val(p, "1 < 2") == 1);
    CHECK(val(p, "2 <= 2") == 1);
    CHECK(val(p, "3 > 4") == 0);
    CHECK(val(p, "4 >= 4") == 1);
    CHECK(val(p, "4 == 4") == 1);
    CHECK(val(p, "4 != 4") == 0);
    CHECK(val(p, "1 && 0") == 0);
    CHECK(val(p, "1 || 0") == 1);
    CHECK(val(p, "1 ? 10 : 20") == 10);
    CHECK(val(p, "0 ? 10 : 20") == 20);
    CHECK(val(p, "1 ? 2 ? 3 : 4 : 5") == 3);
    CHECK(val(p, "(1, 2)") == 2);
    CHECK(val(p, "1 + 2 << 3") == 24);
    CHECK(val(p, "1 | 2 & 3 ^ 4") == (1 | ((2 & 3) ^ 4)));
    CHECK(val(p, "(2 * 676 + 7) / 8") == 169);
    CHECK(val(p, "676 * 2 / 3 + 1") == 451);
}

TEST_CASE("consteval: integer types, promotions and wrap-around") {
    auto prog = parseText("");
    Program& p = *prog;
    auto v = eval(p, "1");
    CHECK(v->bits == 32);
    CHECK(v->isSigned);
    v = eval(p, "1u");
    CHECK(v->bits == 32);
    CHECK(!v->isSigned);
    v = eval(p, "1ULL");
    CHECK(v->bits == 64);
    CHECK(!v->isSigned);
    v = eval(p, "0xFFFFFFFF");
    CHECK(v->bits == 32);
    CHECK(!v->isSigned);
    CHECK(v->value == 4294967295ll);
    v = eval(p, "4294967296");
    CHECK(v->bits == 64);
    CHECK(v->isSigned);
    v = eval(p, "0x7fffffffffffffffi64");
    CHECK(v->bits == 64);
    CHECK(v->value == INT64_MAX);
    v = eval(p, "18446744073709551615ui64");
    CHECK(v->bits == 64);
    CHECK(!v->isSigned);
    CHECK(v->asUnsigned() == UINT64_MAX);
    CHECK(val(p, "-1 < 1u") == 0); // -1 converts to unsigned
    CHECK(val(p, "-1 < 1") == 1);
    CHECK(val(p, "2147483647 + 1") == -2147483648ll); // wraps like the compiler would (UB, but deterministic here)
    CHECK(val(p, "0u - 1") == 4294967295ll);
    CHECK(val(p, "1 << 31") == -2147483648ll);
    CHECK(val(p, "1ULL << 63") == INT64_MIN);
    CHECK(val(p, "'a'") == 97);
    CHECK(val(p, "'\\n'") == 10);
    CHECK(val(p, "'\\x41'") == 65);
    CHECK(val(p, "'\\0'") == 0);
    CHECK(val(p, "1'000'000") == 1000000);
    CHECK(val(p, "0b101") == 5);
    CHECK(val(p, "010") == 8);
    CHECK(val(p, "true + true") == 2);
    CHECK(val(p, "sizeof(int)") == 4);
    CHECK(val(p, "sizeof(long long) * 2") == 16);
    CHECK(val(p, "sizeof(char) + sizeof(short)") == 3);
    CHECK(val(p, "alignof(long long)") == 8);
    CHECK(val(p, "sizeof(unsigned __int8)") == 1);
    CHECK(val(p, "sizeof(signed __int16)") == 2);
    CHECK(val(p, "sizeof(__int64)") == 8);
    CHECK(val(p, "sizeof(long)") == 4); // LLP64 default
    CHECK(val(p, "sizeof(int*)") == 8);
    CHECK(val(p, "sizeof(int[10])") == 40);
}

TEST_CASE("consteval: long follows ProgramOptions::longIs64") {
    ProgramOptions o;
    o.longIs64 = true;
    auto prog = parseText("", nullptr, o);
    CHECK(val(*prog, "sizeof(long)") == 8);
    CHECK(val(*prog, "sizeof(unsigned long)") == 8);
    CHECK(val(*prog, "sizeof(wchar_t)") == 4);
    auto prog2 = parseText("");
    CHECK(val(*prog2, "sizeof(wchar_t)") == 2);
}

TEST_CASE("consteval: casts") {
    auto prog = parseText("typedef unsigned char uint8; enum Color { Red, Green = 5 };");
    Program& p = *prog;
    CHECK(val(p, "(uint8)300") == 44);
    CHECK(val(p, "(unsigned char)255 + 1") == 256);
    CHECK(val(p, "static_cast<uint8>(257)") == 1);
    CHECK(val(p, "static_cast<unsigned long long>(-1)") == -1);
    CHECK(eval(p, "static_cast<unsigned long long>(-1)")->bits == 64);
    CHECK(val(p, "uint8(511)") == 255);
}

TEST_CASE("consteval: floating literals and unsupported constructs fail without crashing") {
    auto prog = parseText("");
    Program& p = *prog;
    std::string err;
    CHECK(!eval(p, "1.5", "", &err).has_value());
    CHECK(!err.empty());
    CHECK(!eval(p, "1 / 0", "", &err).has_value());
    CHECK(err.find("division") != std::string::npos);
    CHECK(!eval(p, "unknown_name + 1", "", &err).has_value());
    CHECK(err.find("unknown") != std::string::npos);
    CHECK(!eval(p, "(", "", &err).has_value());
    CHECK(!eval(p, "1 +", "", &err).has_value());
    CHECK(!eval(p, "1 2", "", &err).has_value());
    CHECK(!eval(p, "\"abc\"", "", &err).has_value());
    CHECK(!eval(p, "foo(1)", "", &err).has_value());
    CHECK(!eval(p, "1 << 99", "", &err).has_value());
    CHECK(!eval(p, "sizeof(NoSuchType)", "", &err).has_value());
    CHECK(!eval(p, "1", "No::Such::Scope", &err).has_value());
    // unevaluated branches are not diagnosed
    CHECK(val(p, "1 ? 5 : 1 / 0") == 5);
    CHECK(val(p, "0 && (1 / 0)") == 0);
    CHECK(val(p, "1 || unknown_name") == 1);
}

TEST_CASE("consteval: names - constexpr variables, enumerators, namespaces, class scope, usings") {
    auto prog = parseText(R"(
        namespace A { constexpr unsigned long long N = 40; namespace B { constexpr int M = N * 2; } }
        using namespace A;
        constexpr int G = B::M + 1;
        enum E { E0, E1, E2 = 10, E3 };
        enum class S : unsigned char { X = 1, Y, Z = Y + 5 };
        struct K {
            static constexpr int kSize = 8;
            enum { kFlag = 1 << 3, kMask = kFlag - 1 };
            static const int kTwice = kSize * 2;
            struct Inner { static constexpr int v = kSize + 1; };
        };
        struct D : K { static constexpr int w = kSize + 100; };
        constexpr int viaOrder = kLater + 1;
        constexpr int kLater = 4;
    )");
    Program& p = *prog;
    CHECK(val(p, "A::N") == 40);
    CHECK(val(p, "A::B::M") == 80);
    CHECK(val(p, "G") == 81);
    CHECK(val(p, "E3") == 11);
    CHECK(val(p, "E2 + E1") == 11);
    CHECK(val(p, "S::Z") == 7);
    CHECK(val(p, "K::kSize") == 8);
    CHECK(val(p, "K::kMask") == 7);
    CHECK(val(p, "K::kTwice") == 16);
    CHECK(val(p, "K::Inner::v") == 9);
    CHECK(val(p, "D::w") == 108);
    CHECK(val(p, "D::kSize") == 8);          // inherited
    CHECK(val(p, "kSize", "K") == 8);        // class scope
    CHECK(val(p, "v", "K::Inner") == 9);     // nested class sees the enclosing class
    CHECK(val(p, "B::M", "") == 80);         // using namespace A
    CHECK(val(p, "viaOrder") == 5);          // use before declaration is resolved lazily
    CHECK(val(p, "sizeof(K::Inner)") == 1);
    CHECK(val(p, "sizeof(E)") == 4);
    CHECK(val(p, "sizeof(S)") == 1);
}

TEST_CASE("consteval: template arguments, '<' disambiguation and '>>'") {
    auto prog = parseText(R"(
        template <int N> struct V { static constexpr int value = N * 3; };
        template <typename T, int N> struct W { static constexpr int value = sizeof(T) * N; };
        template <typename T> struct Box { T x; };
        constexpr int a = 3, b = 5;
        constexpr int lt = a < b;
        constexpr int g1 = V<(a > b)>::value;
        constexpr int g2 = V<a + 1>::value;
        constexpr int g3 = V<(1 > 2 ? 4 : 5)>::value;
        constexpr int nest = sizeof(Box<Box<int>>);
        constexpr int shift = 256 >> 4;
        constexpr int cmp = V<2>::value < 7;
        constexpr int gt = V<2>::value > 5;
    )");
    Program& p = *prog;
    CHECK(val(p, "lt") == 1);
    CHECK(val(p, "g1") == 0);
    CHECK(val(p, "g2") == 12);
    CHECK(val(p, "g3") == 15);
    CHECK(val(p, "nest") == 4);
    CHECK(val(p, "shift") == 16);
    CHECK(val(p, "cmp") == 1);
    CHECK(val(p, "gt") == 1);
    CHECK(val(p, "V<3>::value") == 9);
    CHECK(val(p, "W<long long, 4>::value") == 32);
    CHECK(val(p, "W<Box<Box<char>>, 2>::value") == 2);
    CHECK(val(p, "sizeof(Box<Box<int>>)") == 4);
    CHECK(val(p, "V<1>::value + V<2>::value") == 9);
    CHECK(val(p, "V<1>::value >> 1") == 1);
}

TEST_CASE("consteval: constexpr function calls with simple bodies") {
    auto prog = parseText(R"(
        namespace Q {
            template <typename T> inline static constexpr T div(T a, T b) { return b ? (a / b) : T(0); }
            constexpr int twice(int x) { return x * 2; }
            constexpr int clamp(int x, int lo, int hi) { if (x < lo) return lo; if (x > hi) return hi; return x; }
            constexpr int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
            constexpr int viaLocal(int x) { const int y = x + 1; return y * y; }
        }
        using namespace Q;
        constexpr unsigned short A = div<unsigned short>(1000, 2);
        constexpr int B = div(7, 2);
        constexpr int C = div(7, 0);
    )");
    Program& p = *prog;
    CHECK(val(p, "A") == 500);
    CHECK(eval(p, "A")->bits == 16);
    CHECK(val(p, "B") == 3);
    CHECK(val(p, "C") == 0);
    CHECK(val(p, "twice(21)") == 42);
    CHECK(val(p, "Q::twice(5) + twice(1)") == 12);
    CHECK(val(p, "clamp(50, 0, 10)") == 10);
    CHECK(val(p, "clamp(-5, 0, 10)") == 0);
    CHECK(val(p, "clamp(5, 0, 10)") == 5);
    CHECK(val(p, "fact(6)") == 720);
    CHECK(val(p, "viaLocal(4)") == 25);
    CHECK(val(p, "Q::div<unsigned long long>(100ULL, 8ULL)") == 12);
    CHECK(!eval(*prog, "twice()").has_value());
}

TEST_CASE("consteval: sizeof of expressions and variables") {
    auto prog = parseText(R"(
        struct Row { char name[8]; unsigned short a; unsigned long long b; };
        constexpr Row rows[] = { {"x", 1, 2}, {"y", 3, 4}, {"z", 5, 6} };
        constexpr unsigned count = sizeof(rows) / sizeof(rows[0]);
        struct S { int a; char b[7]; };
        constexpr int sa = sizeof(S::a);
        constexpr int sb = sizeof(S::b);
    )");
    Program& p = *prog;
    CHECK(val(p, "count") == 3);
    CHECK(val(p, "sizeof(Row)") == 24);
    CHECK(val(p, "sizeof(rows)") == 72);
    CHECK(val(p, "sa") == 4);
    CHECK(val(p, "sb") == 7);
    CHECK(val(p, "sizeof(1 + 1)") == 4);
    CHECK(val(p, "sizeof(1ULL + 1)") == 8);
    CHECK(val(p, "sizeof 'a'") == 1);
}

TEST_CASE("consteval: initializer evaluation of aggregate tables") {
    auto prog = parseText(R"(
        struct Desc { char assetName[8]; unsigned short from, to; unsigned long long size; };
        struct Q1 { int a; int b; };
        constexpr struct Desc2 { char n[4]; int v; } tab[] = { {"", 0}, {"QX", 66}, {"AB", 1 + 2 * 3} };
        constexpr Desc descs[] = {
            {"", 0, 0, sizeof(Q1)},
            {"QX", 66, 10000, sizeof(Q1) * 100},
            {"BAD", 1, 2, undefinedName},
        };
        constexpr int plain = 5;
        constexpr int arr[3] = {1, 2, 3};
    )");
    Program& p = *prog;
    auto v = p.lookupVariable("descs");
    REQUIRE(v.has_value());
    CHECK(v->isConstexpr);
    auto init = p.evalInitializer(*v);
    REQUIRE(init.has_value());
    REQUIRE(init->items.size() == 3);
    const InitValue& r1 = init->items[1];
    REQUIRE(r1.items.size() == 4);
    CHECK(r1.items[0].kind == InitValue::Kind::Str);
    CHECK(r1.items[0].s == "QX");
    CHECK(r1.items[1].i == 66);
    CHECK(r1.items[2].i == 10000);
    CHECK(r1.items[3].i == 800);
    CHECK(r1.names[0] == "assetName");
    CHECK(r1.names[3] == "size");
    CHECK(init->items[0].items[3].i == 8);
    CHECK(init->items[2].items[3].kind == InitValue::Kind::Unknown);
    CHECK(init->items[2].items[3].error.find("unknown") != std::string::npos);

    auto t = p.lookupVariable("tab");
    REQUIRE(t.has_value());
    auto ti = p.evalInitializer(*t);
    REQUIRE(ti.has_value());
    REQUIRE(ti->items.size() == 3);
    CHECK(ti->items[2].items[1].i == 7);
    CHECK(p.layoutOf(t->type).count == 3);

    auto a = p.lookupVariable("arr");
    auto ai = p.evalInitializer(*a);
    REQUIRE(ai.has_value());
    REQUIRE(ai->items.size() == 3);
    CHECK(ai->items[2].i == 3);
    auto pl = p.lookupVariable("plain");
    auto pi = p.evalInitializer(*pl);
    REQUIRE(pi.has_value());
    CHECK(pi->kind == InitValue::Kind::Int);
    CHECK(pi->i == 5);
    CHECK(p.variables().size() == 4);
}

TEST_CASE("consteval: absurd nesting is rejected instead of overflowing the stack") {
    auto prog = parseText("");
    std::string deep(5000, '(');
    deep += "1";
    deep += std::string(5000, ')');
    std::string err;
    CHECK(!prog->evalConstant(deep, "", &err).has_value());
    CHECK(err.find("nesting") != std::string::npos);
    std::string unary(5000, '-');
    unary += "1";
    CHECK(!prog->evalConstant(unary, "", &err).has_value());
    std::string tmpl;
    for (int i = 0; i < 2000; ++i) tmpl += "A<";
    CHECK(prog->lookupType(tmpl) == kNoType);
}
