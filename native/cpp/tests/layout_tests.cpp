// Layout engine: scalar sizes, padding, arrays, unions, bases, enums, bit-fields, templates, instantiation.
#include <doctest/doctest.h>

#include "program_test_util.h"
#include "qstate/cpp/preprocessor.h"

using namespace qstate::cpp;
using namespace qstate::cpp::testutil;

namespace {

const TypeLayout& layout(Program& p, std::string_view name, std::string_view scope = {}) {
    TypeId id = p.lookupType(name, scope);
    INFO("type: " << name);
    REQUIRE(id != kNoType);
    const TypeLayout& l = p.layoutOf(id);
    INFO("layout error: " << l.error);
    REQUIRE(l.complete);
    return l;
}

} // namespace

TEST_CASE("layout: scalars, padding and trailing padding") {
    auto prog = parseText(R"(
        struct A { char a; int b; char c; };
        struct B { char a; char b; short c; int d; unsigned long long e; char f; };
        struct C { unsigned long long x; char y; };
        struct D { bool a; signed char b; unsigned char c; wchar_t w; short s; long long ll; };
        struct E { __int8 a; unsigned __int16 b; signed __int32 c; unsigned __int64 d; };
        struct F { long long unsigned int a; short int b; unsigned short int c; long int d; unsigned long e; };
        struct P { int* p; void* q; const char* name; int (*fn)(int, int); };
    )");
    Program& p = *prog;
    const TypeLayout& a = layout(p, "A");
    CHECK(a.size == 12);
    CHECK(a.align == 4);
    CHECK(offsetOf(a, "b") == 4);
    CHECK(offsetOf(a, "c") == 8);
    const TypeLayout& b = layout(p, "B");
    CHECK(offsetOf(b, "c") == 2);
    CHECK(offsetOf(b, "d") == 4);
    CHECK(offsetOf(b, "e") == 8);
    CHECK(offsetOf(b, "f") == 16);
    CHECK(b.size == 24);
    CHECK(layout(p, "C").size == 16);
    const TypeLayout& d = layout(p, "D");
    CHECK(offsetOf(d, "b") == 1);
    CHECK(offsetOf(d, "w") == 4);
    CHECK(offsetOf(d, "s") == 6);
    CHECK(offsetOf(d, "ll") == 8);
    CHECK(d.size == 16);
    const TypeLayout& e = layout(p, "E");
    CHECK(offsetOf(e, "b") == 2);
    CHECK(offsetOf(e, "c") == 4);
    CHECK(offsetOf(e, "d") == 8);
    CHECK(e.size == 16);
    const TypeLayout& f = layout(p, "F");
    CHECK(offsetOf(f, "b") == 8);
    CHECK(offsetOf(f, "c") == 10);
    CHECK(offsetOf(f, "d") == 12);  // long = 4 (LLP64)
    CHECK(offsetOf(f, "e") == 16);
    CHECK(f.size == 24);
    const TypeLayout& pt = layout(p, "P");
    CHECK(pt.size == 32);
    CHECK(offsetOf(pt, "fn") == 24);
    CHECK(pt.fields[0].declaredType == "int*");
}

TEST_CASE("layout: arrays with constant-expression extents, nested arrays, typedef chains") {
    auto prog = parseText(R"(
        typedef unsigned long long uint64;
        typedef uint64 big_t;
        typedef big_t lots_t[3];
        constexpr uint64 N = 4;
        enum { kRows = 2, kCols = N + 1 };
        struct M { char tag; big_t grid[kRows][kCols]; lots_t l; char tail[sizeof(big_t) * 2 + 1]; };
        struct Z { int a; char none[0]; };
    )");
    Program& p = *prog;
    const TypeLayout& m = layout(p, "M");
    CHECK(offsetOf(m, "grid") == 8);
    CHECK(m.fields[1].size == 8 * 2 * 5);
    CHECK(offsetOf(m, "l") == 88);
    CHECK(m.fields[2].size == 24);
    CHECK(offsetOf(m, "tail") == 112);
    CHECK(m.fields[3].size == 17);
    CHECK(m.size == 136);
    CHECK(m.fields[1].declaredType == "big_t[kRows][kCols]");
    CHECK(layout(p, "Z").size == 4);
    // canonical identity
    CHECK(p.lookupType("big_t") == p.lookupType("uint64"));
    CHECK(p.lookupType("unsigned long long") == p.lookupType("uint64"));
}

TEST_CASE("layout: empty types, empty bases and bases with members (MSVC rule)") {
    auto prog = parseText(R"(
        struct Empty {};
        struct HasEmpty { Empty e; unsigned x; };
        struct DerivedFromEmpty : Empty { unsigned x; };
        struct OnlyEmptyBase : Empty {};
        struct Base5 { int a; char b; };            // size 8, tail padding 3
        struct Derived : Base5 { char c; };         // MSVC: c at 8, size 12
        struct Derived2 : public Base5 { int d; char e; };
        struct Two { int a; };
        struct Multi : Two, Base5 { char z; };
    )");
    Program& p = *prog;
    CHECK(layout(p, "Empty").size == 1);
    CHECK(layout(p, "Empty").isEmpty);
    const TypeLayout& he = layout(p, "HasEmpty");
    CHECK(offsetOf(he, "x") == 4);
    CHECK(he.size == 8);
    const TypeLayout& dfe = layout(p, "DerivedFromEmpty");
    CHECK(dfe.size == 4);
    CHECK(offsetOf(dfe, "x") == 0);
    REQUIRE(dfe.bases.size() == 1);
    CHECK(dfe.bases[0].offset == 0);
    CHECK(layout(p, "OnlyEmptyBase").size == 1);
    const TypeLayout& d = layout(p, "Derived");
    CHECK(offsetOf(d, "c") == 8);
    CHECK(d.size == 12);
    const TypeLayout& d2 = layout(p, "Derived2");
    CHECK(offsetOf(d2, "d") == 8);
    CHECK(offsetOf(d2, "e") == 12);
    CHECK(d2.size == 16);
    const TypeLayout& mu = layout(p, "Multi");
    REQUIRE(mu.bases.size() == 2);
    CHECK(mu.bases[0].offset == 0);
    CHECK(mu.bases[1].offset == 4);
    CHECK(offsetOf(mu, "z") == 12);
    CHECK(mu.size == 16);
}

TEST_CASE("layout: unions, anonymous unions and structs, named inline types") {
    auto prog = parseText(R"(
        union U { char c; int i; unsigned long long ll; char buf[12]; };
        struct W {
            int tag;
            union { int a; char b[6]; };          // anonymous union member
            struct { short x, y; } pos;           // inline struct with declarator
            union Data { char c; unsigned long long v; } data;
            struct { char q; };                   // anonymous struct member
        };
        union Wide { struct { unsigned long long a, b; } two; char raw[10]; };
    )");
    Program& p = *prog;
    const TypeLayout& u = layout(p, "U");
    CHECK(u.recordKind == RecordKind::Union);
    CHECK(u.size == 16);
    CHECK(u.align == 8);
    CHECK(offsetOf(u, "buf") == 0);
    const TypeLayout& w = layout(p, "W");
    CHECK(offsetOf(w, "pos") == 12);
    CHECK(offsetOf(w, "data") == 16);
    CHECK(w.size == 32);
    REQUIRE(w.fields.size() == 5);
    CHECK(w.fields[1].name.empty());
    CHECK(w.fields[1].size == 8);
    CHECK(offsetOf(w, "") == 4);
    CHECK(layout(p, "W::Data").size == 8);
    CHECK(layout(p, "Wide").size == 16);
}

TEST_CASE("layout: enums") {
    auto prog = parseText(R"(
        enum Plain { A, B, C };
        enum Big { Huge = 0x7fffffff };
        enum Bigger { Huger = 0x80000000u };
        enum class Small : unsigned char { X = 1, Y };
        enum Typed : short { T1 = -1, T2 };
        enum class Wide : unsigned long long { W = 1ull << 40 };
        struct S { Small s; char c; Typed t; Wide w; Plain p; };
        typedef unsigned char uint8;
        enum class ViaTypedef : uint8 { V };
    )");
    Program& p = *prog;
    CHECK(layout(p, "Plain").size == 4);
    CHECK(layout(p, "Big").size == 4);
    CHECK(layout(p, "Bigger").size == 4);
    const TypeLayout& s = layout(p, "Small");
    CHECK(s.size == 1);
    CHECK(s.scopedEnum);
    REQUIRE(s.enumerators.size() == 2);
    CHECK(s.enumerators[1].name == "Y");
    CHECK(s.enumerators[1].value == 2);
    CHECK(layout(p, "Typed").size == 2);
    CHECK(layout(p, "Typed").enumerators[0].value == -1);
    CHECK(layout(p, "Wide").size == 8);
    const TypeLayout& st = layout(p, "S");
    CHECK(offsetOf(st, "t") == 2);
    CHECK(offsetOf(st, "w") == 8);
    CHECK(offsetOf(st, "p") == 16);
    CHECK(st.size == 24);
    CHECK(layout(p, "ViaTypedef").size == 1);
}

TEST_CASE("layout: bit-fields follow the MSVC allocation rule") {
    auto prog = parseText(R"(
        struct B1 { unsigned a : 3; unsigned b : 5; unsigned c : 24; unsigned d : 1; };
        struct B2 { unsigned char a : 4; unsigned b : 4; };      // different base type sizes start a new unit
        struct B3 { char a; int b : 3; char c; };
        struct B4 { unsigned a : 4; unsigned : 0; unsigned b : 4; };
    )");
    Program& p = *prog;
    const TypeLayout& b1 = layout(p, "B1");
    CHECK(b1.size == 8);
    CHECK(b1.fields[0].bitOffset == 0);
    CHECK(b1.fields[0].bitWidth == 3);
    CHECK(b1.fields[1].bitOffset == 3);
    CHECK(b1.fields[2].bitOffset == 8);
    CHECK(b1.fields[3].offset == 4);
    CHECK(b1.fields[3].bitOffset == 0);
    const TypeLayout& b2 = layout(p, "B2");
    CHECK(b2.fields[1].offset == 4);
    CHECK(b2.size == 8);
    const TypeLayout& b3 = layout(p, "B3");
    CHECK(b3.fields[1].offset == 4);
    CHECK(offsetOf(b3, "c") == 8);
    CHECK(b3.size == 12);
    const TypeLayout& b4 = layout(p, "B4");
    CHECK(b4.fields[1].offset == 4);
    CHECK(b4.size == 8);
}

TEST_CASE("layout: class templates, defaults, nested types of instances") {
    auto prog = parseText(R"(
        namespace N {
            template <typename T, unsigned long long L> struct Arr {
                static constexpr unsigned long long capacity = L;
                struct Slot { T value; unsigned char used; };
                typedef Slot SlotT;
                T _values[L];
                Slot slots[L / 2 + 1];
            };
            template <typename K, typename V, unsigned long long L, typename H = K> struct Map {
                Arr<K, L> keys;
                Arr<V, L> values;
                H hint;
                typename Arr<V, L>::SlotT last;
                static_assert(L > 0, "positive");
            };
        }
        struct Item { int a; char b; };
        struct Holder {
            N::Arr<Item, 3> items;
            N::Map<char, Item, 4> map;
            N::Map<char, Item, 4, long long> map2;
        };
    )");
    Program& p = *prog;
    const TypeLayout& arr = layout(p, "N::Arr<Item, 3>");
    CHECK(arr.size == 3 * 8 + 2 * 12);
    CHECK(arr.templateName == "N::Arr");
    REQUIRE(arr.templateArgs.size() == 2);
    CHECK(arr.templateArgs[0].isType);
    CHECK(arr.templateArgs[0].text == "Item");
    CHECK(arr.templateArgs[1].text == "3");
    CHECK(arr.name == "N::Arr<Item, 3>");
    // nested type of an instance is specific to the instance
    CHECK(layout(p, "N::Arr<Item, 3>::Slot").size == 12);
    CHECK(layout(p, "N::Arr<char, 3>::Slot").size == 2);
    CHECK(layout(p, "N::Arr<char, 3>::Slot").name == "N::Arr<char, 3>::Slot");
    CHECK(p.lookupType("N::Arr<char, 3>::Slot") != p.lookupType("N::Arr<Item, 3>::Slot"));
    CHECK(layout(p, "N::Arr<Item, 3>::Slot").enclosing == p.lookupType("N::Arr<Item, 3>"));
    CHECK(p.lookupType("N::Arr<Item,3>") == p.lookupType("N::Arr<Item, 3>"));
    CHECK(p.lookupType("N::Arr<Item, 1+2>") == p.lookupType("N::Arr<Item, 3>"));
    CHECK(p.lookupType("N::Arr<Item, 3ULL>") == p.lookupType("N::Arr<Item, 3>"));
    const TypeLayout& m = layout(p, "N::Map<char, Item, 4>");
    CHECK(m.name == "N::Map<char, Item, 4, char>");
    CHECK(p.lookupType("N::Map<char, Item, 4, char>") == p.lookupType("N::Map<char, Item, 4>"));
    CHECK(m.fields[0].size == 10);
    CHECK(offsetOf(m, "values") == 12); // Arr<Item, 4> has alignment 4
    CHECK(layout(p, "N::Map<char, Item, 4, long long>").size > m.size - 1);
    const TypeLayout& h = layout(p, "Holder");
    CHECK(h.fields.size() == 3);
    CHECK(offsetOf(h, "map") == h.fields[0].size);
    CHECK(h.fields[0].declaredType == "N::Arr<Item, 3>");
    CHECK(p.evalConstant("N::Arr<Item, 3>::capacity").value().value == 3);
    CHECK(p.evalConstant("N::Map<char, Item, 4>::capacity").has_value() == false);
}

TEST_CASE("layout: explicit and partial specializations, alias templates, dependent names") {
    auto prog = parseText(R"(
        template <bool B> struct Sel { typedef char type; };
        template <> struct Sel<true> { typedef long long type; };
        template <typename T, unsigned N> struct Vote { T data[N]; static constexpr bool scalar = false; };
        template <unsigned N> struct Vote<unsigned char, N> { unsigned char bits[(2 * N + 7) / 8]; static constexpr bool scalar = true; };
        template <typename T> struct Base { T value; static constexpr int kind = 1; };
        template <typename T> struct Derived : public Base<T> { char extra; };
        template <typename P> struct UsesParam : public P { char more; };
        struct Plain { short s; };
        template <typename T> using Pair = Vote<T, 2>;
        template <typename T> struct Dep { typename Sel<(sizeof(T) > 4)>::type big; Sel<true>::type fixed; };
        template <typename T> struct Fwd;
        template <typename T> struct Fwd { T x; };
        struct Outer { struct Inner { char c; } i; typedef Inner InnerT; };
        struct Other { struct Inner { long long ll; } i; };
    )");
    Program& p = *prog;
    CHECK(layout(p, "Sel<false>::type").size == 1);
    CHECK(layout(p, "Sel<true>::type").size == 8);
    CHECK(layout(p, "Vote<int, 4>").size == 16);
    CHECK(layout(p, "Vote<int, 4>").fields.size() == 1);
    CHECK(layout(p, "Vote<unsigned char, 676>").size == 169);
    CHECK(p.evalConstant("Vote<unsigned char, 3>::scalar").value().value == 1);
    CHECK(p.evalConstant("Vote<int, 3>::scalar").value().value == 0);
    const TypeLayout& d = layout(p, "Derived<int>");
    CHECK(d.size == 8);
    CHECK(offsetOf(d, "extra") == 4);
    CHECK(p.evalConstant("Derived<int>::kind").value().value == 1); // inherited static member of an instance
    CHECK(layout(p, "Derived<long long>").size == 16);
    const TypeLayout& up = layout(p, "UsesParam<Plain>");
    CHECK(up.size == 4);
    CHECK(offsetOf(up, "more") == 2);
    CHECK(layout(p, "Pair<short>").size == 4);
    CHECK(layout(p, "Pair<unsigned char>").size == 1);
    const TypeLayout& dep = layout(p, "Dep<char>");
    CHECK(dep.fields[0].size == 1);
    CHECK(dep.fields[1].size == 8);
    CHECK(offsetOf(dep, "fixed") == 8);
    CHECK(layout(p, "Dep<long long>").fields[0].size == 8);
    CHECK(layout(p, "Fwd<short>").size == 2);
    // same nested name in different scopes does not collide
    CHECK(layout(p, "Outer::Inner").size == 1);
    CHECK(layout(p, "Other::Inner").size == 8);
    CHECK(layout(p, "Outer::InnerT").size == 1);
    CHECK(p.lookupType("Outer::InnerT") == p.lookupType("Outer::Inner"));
}

TEST_CASE("layout: declaration order does not matter inside classes and for templates") {
    auto prog = parseText(R"(
        struct First { Later l; Later::Deep d; static constexpr int kN = kM + 1; char arr[kN]; static constexpr int kM = 2; };
        struct Later { struct Deep { int z; }; int q; };
        template <int N> struct T { Body b; struct Body { char x[N]; }; };
    )");
    Program& p = *prog;
    // Later::Deep is a type (not a member): `Later::Deep d;` declares a field
    const TypeLayout& f = layout(p, "First");
    CHECK(f.fields.size() == 3);
    CHECK(offsetOf(f, "arr") == 8);
    CHECK(f.size == 12);
    CHECK(layout(p, "T<5>").size == 5);
}

TEST_CASE("layout: diagnostics instead of exceptions for incomplete and recursive types") {
    Diags diags;
    auto prog = parseText(R"(
        struct Fwd;
        struct HasFwd { Fwd f; };
        struct PtrFwd { Fwd* f; int x; };
        struct Rec { Rec r; };
        struct Self { Self* next; int v; };
        template <int N> struct Deep { Deep<N + 1> d; };
        struct Bad { Unknown u; };
        struct Ok { int a; };
    )", &diags);
    Program& p = *prog;
    TypeId hf = p.lookupType("HasFwd");
    REQUIRE(hf != kNoType);
    CHECK(!p.layoutOf(hf).complete);
    CHECK(p.layoutOf(p.lookupType("PtrFwd")).size == 16);
    CHECK(!p.layoutOf(p.lookupType("Rec")).complete);
    CHECK(p.layoutOf(p.lookupType("Self")).size == 16);
    CHECK(!p.layoutOf(p.lookupType("Deep<0>")).complete); // instantiation depth limit
    CHECK(!p.layoutOf(p.lookupType("Bad")).complete);
    CHECK(p.layoutOf(p.lookupType("Ok")).size == 4);
    CHECK(p.lookupType("DoesNotExist") == kNoType);
    CHECK(p.lookupType("Ok<3>") == kNoType);
    CHECK(p.lookupType("Ok::Nothing") == kNoType);
    CHECK(!p.diags().empty());
    CHECK(p.layoutOf(kNoType).complete == false);
    CHECK(p.layoutOf(123456).complete == false);
}

TEST_CASE("layout: static_assert is evaluated and failures become diagnostics") {
    Diags diags;
    auto prog = parseText(R"(
        struct S { int a; };
        static_assert(sizeof(S) == 4, "ok");
        static_assert(sizeof(S) == 8, "wrong size");
        static_assert(sizeof(Missing) == 1);
        struct T { static_assert(sizeof(S) == 4); int b; static_assert(sizeof(S) > 100, "member"); };
        template <int N> struct U { static_assert(N > 5, "N too small"); int x[N]; };
    )", &diags);
    int warnings = 0;
    int notes = 0;
    for (const Diag& d : diags) {
        if (d.severity == Diag::Severity::Warning) ++warnings;
        if (d.severity == Diag::Severity::Note) ++notes;
    }
    INFO(text(diags));
    CHECK(warnings == 2); // wrong size + member assert
    CHECK(notes >= 1);    // Missing
    Program& p = *prog;
    CHECK(layout(p, "U<10>").size == 40);
    const std::size_t before = p.diags().size();
    CHECK(layout(p, "U<2>").size == 8);
    CHECK(p.diags().size() > before); // "N too small" reported when the instance is laid out
}

TEST_CASE("layout: TypeLayout metadata of records, arrays and pointers") {
    auto prog = parseText(R"(
        namespace ns { struct R { int a[3]; char* p; }; }
    )");
    Program& p = *prog;
    const TypeLayout& r = layout(p, "ns::R");
    CHECK(r.name == "ns::R");
    CHECK(r.baseName == "R");
    CHECK(r.kind == TypeKind::Record);
    CHECK(r.loc.line == 2);
    CHECK(r.fields[0].loc.line == 2);
    const TypeLayout& arr = p.layoutOf(r.fields[0].type);
    CHECK(arr.kind == TypeKind::Array);
    CHECK(arr.count == 3);
    CHECK(p.layoutOf(arr.element).kind == TypeKind::Int);
    CHECK(arr.name == "int[3]");
    const TypeLayout& ptr = p.layoutOf(r.fields[1].type);
    CHECK(ptr.kind == TypeKind::Pointer);
    CHECK(p.typeName(r.fields[1].type) == "char*");
    CHECK(p.typeCount() > 5);
    CHECK(p.allTypes().size() == p.typeCount());
    CHECK(p.fileName(0) == "test.h");
}

TEST_CASE("layout: alignas and __declspec(align)") {
    auto prog = parseText(R"(
        struct alignas(16) A16 { char c; };
        struct __declspec(align(32)) A32 { int x; };
        struct F { char a; alignas(8) char b; alignas(long long) char c; char d; };
        struct G { char a; A16 x; char b; };
        struct H { char a; __declspec(align(4)) char b; };
    )");
    Program& p = *prog;
    const TypeLayout& a16 = layout(p, "A16");
    CHECK(a16.align == 16);
    CHECK(a16.size == 16);
    CHECK(layout(p, "A32").size == 32);
    const TypeLayout& f = layout(p, "F");
    CHECK(offsetOf(f, "b") == 8);
    CHECK(offsetOf(f, "c") == 16);
    CHECK(offsetOf(f, "d") == 17);
    CHECK(f.align == 8);
    CHECK(f.size == 24);
    const TypeLayout& g = layout(p, "G");
    CHECK(offsetOf(g, "x") == 16);
    CHECK(offsetOf(g, "b") == 32);
    CHECK(g.size == 48);
    CHECK(offsetOf(layout(p, "H"), "b") == 4);
}

TEST_CASE("layout: #pragma pack regions") {
    std::vector<PragmaRecord> pragmas;
    // token indices follow the lexed text below: we compute them from the token stream
    std::string code = "struct Before { char a; int b; };\n"
                       "#PACK_PUSH\n"
                       "struct Packed { char a; int b; long long c; struct Inner { char x; short y; } i; };\n"
                       "#PACK_POP\n"
                       "struct After { char a; int b; };\n";
    // split at the markers
    std::vector<Token> tokens;
    std::size_t pos = 0;
    std::vector<std::string> segments;
    for (const char* marker : {"#PACK_PUSH\n", "#PACK_POP\n"}) {
        std::size_t q = code.find(marker, pos);
        segments.push_back(code.substr(pos, q - pos));
        pos = q + std::string(marker).size();
    }
    segments.push_back(code.substr(pos));
    for (std::size_t i = 0; i < segments.size(); ++i) {
        std::vector<Token> seg = lexFragment(segments[i]);
        if (i == 1) {
            PragmaRecord r;
            r.text = "pack(push, 1)";
            r.tokenIndex = tokens.size();
            pragmas.push_back(r);
        }
        if (i == 2) {
            PragmaRecord r;
            r.text = "pack(pop)";
            r.tokenIndex = tokens.size();
            pragmas.push_back(r);
        }
        for (Token& t : seg) tokens.push_back(std::move(t));
    }
    const std::uint32_t total = static_cast<std::uint32_t>(tokens.size());
    Token end;
    end.kind = TokKind::End;
    tokens.push_back(end);
    ProgramOptions options;
    options.packRegions = packRegionsFromPragmas(pragmas, total);
    REQUIRE(options.packRegions.size() == 1);
    CHECK(options.packRegions[0].pack == 1);
    Diags diags;
    auto prog = Program::parse(std::move(tokens), {"t.h"}, diags, options);
    Program& p = *prog;
    CHECK(layout(p, "Before").size == 8);
    const TypeLayout& packed = layout(p, "Packed");
    CHECK(offsetOf(packed, "b") == 1);
    CHECK(offsetOf(packed, "c") == 5);
    CHECK(offsetOf(packed, "i") == 13);
    CHECK(packed.size == 16);
    CHECK(packed.align == 1);
    CHECK(layout(p, "Packed::Inner").size == 3);
    CHECK(layout(p, "After").size == 8);
}

TEST_CASE("layout: injected class name with template arguments, unbounded recursion is cut off") {
    auto prog = parseText(R"(
        template <int N> struct Node { Node<N> *self; Node<N + 1> *next; int v[N]; };
        template <int N> struct Deep { Deep<N + 1> d; };
        template <int N> struct Wide { char c[N]; Wide<N - 1> w; };
    )");
    Program& p = *prog;
    const TypeLayout& n = layout(p, "Node<2>");
    CHECK(n.size == 24);
    CHECK(p.layoutOf(n.fields[0].type).kind == TypeKind::Pointer);
    CHECK(p.layoutOf(p.layoutOf(n.fields[1].type).element).name == "Node<3>");
    CHECK(!p.layoutOf(p.lookupType("Deep<0>")).complete);
    CHECK(!p.layoutOf(p.lookupType("Wide<5>")).complete);
    CHECK(p.layoutOf(p.lookupType("Deep<0>")).error.size() < 2000);
}
