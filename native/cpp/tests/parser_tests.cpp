// Declaration parser: scopes, declaration forms, skipping of everything that does not matter, recovery.
#include <doctest/doctest.h>

#include <random>
#include <set>

#include "program_test_util.h"

using namespace qstate::cpp;
using namespace qstate::cpp::testutil;

namespace {

bool hasDecl(Program& p, const std::string& qualified, DeclInfo::Kind kind) {
    for (const DeclInfo& d : p.declarations())
        if (d.qualifiedName == qualified && d.kind == kind) return true;
    return false;
}

std::size_t warnings(const Diags& d) {
    std::size_t n = 0;
    for (const Diag& x : d)
        if (x.severity != Diag::Severity::Note) ++n;
    return n;
}

} // namespace

TEST_CASE("parser: namespaces (nested, reopened, C++17 nested definitions, anonymous, inline)") {
    Diags diags;
    auto prog = parseText(R"(
        namespace A { struct S1 { int a; }; }
        namespace A { namespace B { struct S2 { int b; }; } }
        namespace A::B::C { struct S3 { int c; }; }
        namespace { struct Hidden { int h; }; }
        inline namespace V1 { struct Versioned { int v; }; }
        namespace Alias = A::B;
        using namespace A::B;
        struct UsesUsing { S2 s; C::S3 t; };
        namespace A { namespace B { int x; }; };
    )", &diags);
    Program& p = *prog;
    CHECK(warnings(diags) == 0);
    CHECK(hasDecl(p, "A", DeclInfo::Kind::Namespace));
    CHECK(hasDecl(p, "A::B::C", DeclInfo::Kind::Namespace));
    CHECK(hasDecl(p, "A::B::C::S3", DeclInfo::Kind::Record));
    CHECK(p.layoutOf(p.lookupType("A::B::S2")).size == 4);
    CHECK(p.layoutOf(p.lookupType("Hidden")).size == 4);
    CHECK(p.layoutOf(p.lookupType("Versioned")).size == 4);
    CHECK(p.layoutOf(p.lookupType("UsesUsing")).size == 8);
    CHECK(p.lookupType("S1") == kNoType); // not visible without qualification
    CHECK(p.lookupType("S1", "A") != kNoType);
    CHECK(p.lookupType("S2", "A::B") != kNoType);
    CHECK(p.lookupType("B::S2", "A") != kNoType);
    CHECK(p.lookupType("::A::S1") != kNoType);
}

TEST_CASE("parser: member functions, constructors, operators and bodies are skipped") {
    Diags diags;
    auto prog = parseText(R"(
        template <typename T> struct Helper { T v; };
        struct Fat {
            int a;
            Fat() : a(0), b{1}, c(Helper<int>{2}.v) {}
            explicit Fat(int x) : a(x) {}
            Fat(const Fat& o) = default;
            Fat& operator=(const Fat&) = delete;
            ~Fat() {}
            bool operator==(const Fat& o) const { return a == o.a; }
            bool operator<(const Fat& o) const { return a < o.a; }
            bool operator<<(int) const { return false; }
            Fat operator()(int, int) const { return *this; }
            int operator[](int i) const { return i; }
            operator bool() const { return a != 0; }
            template <typename X> X convert() const { return X(); }
            static int make(int x) { switch (x) { case 1: return 2; default: { goto done; } } done: return 0; }
            inline static void lambda() { auto f = [&](int q) -> int { return q > 3 ? q : -q; }; (void)f(1); }
            void templ() const { Helper<Helper<int>> h{}; unsigned long long z = 5 >> 1; (void)h; (void)z; }
            const int* getP() const noexcept { return &a; }
            auto trailing() const -> int { return 1; }
            friend bool operator!=(const Fat&, const Fat&) { return false; }
            friend struct Helper<int>;
            int b;
            int c;
        public:
            int d;
        private:
            int e;
        protected:
            int f;
        };
        struct After { int z; };
    )", &diags);
    Program& p = *prog;
    INFO(text(diags));
    CHECK(warnings(diags) == 0);
    const TypeLayout& f = p.layoutOf(p.lookupType("Fat"));
    REQUIRE(f.complete);
    CHECK(f.fields.size() == 6);
    CHECK(f.size == 24);
    CHECK(p.layoutOf(p.lookupType("After")).size == 4);
}

TEST_CASE("parser: out-of-class definitions, free functions, function templates, extern \"C\"") {
    Diags diags;
    auto prog = parseText(R"(
        template <typename T, int N> struct Box { T v[N]; int size() const; template <typename U> void put(U u); };
        template <typename T, int N> int Box<T, N>::size() const { return N; }
        template <typename T, int N> template <typename U> void Box<T, N>::put(U u) { v[0] = u; }
        template <typename T> inline bool isZero(const T& x) { return x == T(); }
        template <> inline bool isZero<int>(const int& x) { return x == 0; }
        static inline void* ptrFunc(void** out, unsigned long long size, int flags = 0);
        extern "C" { int cfunc(int); struct CS { int a; }; }
        extern "C" int other(void);
        extern int externVar;
        typedef void (*Callback)(int, void*);
        typedef int (__cdecl* StdCall)(int);
        typedef struct { int a; char b; } Anon, *AnonPtr;
        typedef struct Tagged { int t; } TaggedT;
        struct UsesAnon { Anon x; AnonPtr p; Callback cb; TaggedT tt; };
        struct Last { int z; };
    )", &diags);
    Program& p = *prog;
    INFO(text(diags));
    CHECK(warnings(diags) == 0);
    CHECK(p.layoutOf(p.lookupType("CS")).size == 4);
    const TypeLayout& u = p.layoutOf(p.lookupType("UsesAnon"));
    REQUIRE(u.complete);
    CHECK(u.size == 8 + 8 + 8 + 4 + 4);
    CHECK(p.layoutOf(p.lookupType("Box<char, 5>")).size == 5);
    CHECK(p.layoutOf(p.lookupType("Last")).size == 4);
}

TEST_CASE("parser: static data members, namespace variables, initializer forms") {
    auto prog = parseText(R"(
        constexpr int a = 1, b = a + 1, c[2] = {a, b};
        static constexpr unsigned long long big = 1ull << 40;
        const int x{7};
        int mutableVar = 3;
        struct S {
            static constexpr int k = 3;
            static const int j = k + 1;
            static int unused;
            int field = 5;
            int other{6};
            int arr[k] = {1, 2, 3};
            int bits : 3 = 1;
            unsigned long long ull = 1 << 3, ull2 = (2 > 1) ? 1 : 2;
        };
        constexpr int viaStatic = S::j;
    )");
    Program& p = *prog;
    CHECK(p.evalConstant("b").value().value == 2);
    CHECK(p.evalConstant("big").value().value == (1ll << 40));
    CHECK(p.evalConstant("x").value().value == 7);
    CHECK(p.evalConstant("S::j").value().value == 4);
    CHECK(p.evalConstant("viaStatic").value().value == 4);
    const TypeLayout& s = p.layoutOf(p.lookupType("S"));
    REQUIRE(s.complete);
    CHECK(s.fields.size() == 6);
    CHECK(p.layoutOf(s.fields[2].type).count == 3);
    auto vars = p.variables();
    std::set<std::string> names;
    for (const VariableInfo& v : vars) names.insert(v.name);
    CHECK(names.count("a"));
    CHECK(names.count("c"));
    CHECK(names.count("mutableVar"));
    auto sv = p.variables("S");
    CHECK(sv.size() == 3);
}

TEST_CASE("parser: typedef and using aliases, alias templates, chains across scopes") {
    auto prog = parseText(R"(
        typedef unsigned int uint32;
        using uint32b = uint32;
        using Pair = struct { int a; int b; };
        template <typename T> struct Vec { T d[4]; };
        using Vec4i = Vec<int>;
        template <typename T> using Vec4 = Vec<T>;
        struct Contract {
            typedef Vec4i V;
            using W = Vec4<char>;
            struct StateData { V v; W w; uint32b u; Pair p; };
        };
    )");
    Program& p = *prog;
    const TypeLayout& s = p.layoutOf(p.lookupType("Contract::StateData"));
    REQUIRE(s.complete);
    CHECK(offsetOf(s, "w") == 16);
    CHECK(offsetOf(s, "u") == 20);
    CHECK(offsetOf(s, "p") == 24);
    CHECK(s.size == 32);
    CHECK(p.lookupType("uint32b") == p.lookupType("unsigned int"));
    CHECK(p.lookupType("Vec4i") == p.lookupType("Vec<int>"));
    CHECK(p.lookupType("Vec4<int>") == p.lookupType("Vec<int>"));
}

TEST_CASE("parser: template forward declarations, default arguments merged across redeclarations") {
    auto prog = parseText(R"(
        template <typename T, unsigned long long N = 8> struct Buf;
        struct User { Buf<char> b; };
        template <typename T, unsigned long long M> struct Buf { T data[M]; };
        template <typename T, unsigned long long K> struct Buf;
    )");
    Program& p = *prog;
    CHECK(p.layoutOf(p.lookupType("User")).size == 8);
    CHECK(p.layoutOf(p.lookupType("Buf<int>")).size == 32);
    CHECK(p.lookupType("Buf<int>") == p.lookupType("Buf<int, 8>"));
}

TEST_CASE("parser: attributes, calling conventions and stray tokens are tolerated") {
    Diags diags;
    auto prog = parseText(R"(
        ;
        struct __declspec(align(8)) A { int a; };
        struct B { __declspec(noinline) static int f() { return 1; } int b; };
        extern "C" __declspec(dllexport) int __cdecl exported(int);
        struct C { int c; };;
        void freeFn() { { } ; }
        struct D { int d; } ;
    )", &diags);
    Program& p = *prog;
    INFO(text(diags));
    CHECK(p.layoutOf(p.lookupType("A")).size == 8); // __declspec(align(8))
    CHECK(p.layoutOf(p.lookupType("B")).size == 4);
    CHECK(p.layoutOf(p.lookupType("C")).size == 4);
    CHECK(p.layoutOf(p.lookupType("D")).size == 4);
}

TEST_CASE("parser: malformed input produces diagnostics and recovery") {
    Diags diags;
    auto prog = parseText(R"(
        struct Before { int a; };
        struct Broken { int a; @@@ garbage ) ] ; int b; };
        int ) ( ;
        template <typename T struct NoClose { };
        struct Between { char z; };
        namespace Open { struct InOpen { short s; };
    )", &diags);
    Program& p = *prog;
    CHECK(p.layoutOf(p.lookupType("Before")).size == 4);
    CHECK(p.layoutOf(p.lookupType("Between")).size == 1);
    CHECK(!diags.empty());
}

TEST_CASE("parser: never crashes or hangs on truncated and mutilated input") {
    const std::string sample = R"(
        namespace QPI {
            typedef unsigned long long uint64;
            template <typename T, uint64 L> struct Array { static_assert(L > 0, "x"); T _values[L]; Array() {} void f() { for (int i = 0; i < 3; ++i) { } } };
            template <uint64 N> struct Bits { enum : int { K = N > 3 ? 1 : 2 }; unsigned char b[(N + 7) / 8]; };
        }
        using namespace QPI;
        union U { int a; struct { char c; short s; }; };
        struct S : public U { Array<int, 4> arr; Bits<20> bits; typedef Array<char, 2> T2; T2 t; operator int() const { return 1; } };
        constexpr struct D { char n[4]; int v; } tab[] = { {"a", 1}, {"b", sizeof(S)} };
        enum class E : unsigned char { A = 1, B = A + 1 };
    )";
    std::vector<Token> all = lex(sample, 0);
    REQUIRE(all.size() > 100);
    // prefixes
    for (std::size_t n = 0; n + 1 < all.size(); n += 3) {
        std::vector<Token> t(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n));
        Token end;
        end.kind = TokKind::End;
        t.push_back(end);
        Diags diags;
        auto prog = Program::parse(std::move(t), {"t.h"}, diags);
        TypeId id = prog->lookupType("S");
        if (id != kNoType) (void)prog->layoutOf(id);
        (void)prog->evalConstant("sizeof(S) + 1");
    }
    // random token deletions / swaps
    std::mt19937 rng(12345);
    for (int iter = 0; iter < 300; ++iter) {
        std::vector<Token> t = all;
        const int edits = 1 + static_cast<int>(rng() % 4);
        for (int e = 0; e < edits && t.size() > 3; ++e) {
            std::size_t i = rng() % (t.size() - 1);
            switch (rng() % 3) {
            case 0: t.erase(t.begin() + static_cast<std::ptrdiff_t>(i)); break;
            case 1: std::swap(t[i], t[rng() % (t.size() - 1)]); break;
            default: t.insert(t.begin() + static_cast<std::ptrdiff_t>(i), t[rng() % (t.size() - 1)]); break;
            }
        }
        Diags diags;
        auto prog = Program::parse(std::move(t), {"t.h"}, diags);
        for (const char* name : {"S", "U", "QPI::Array<int, 4>", "D", "E"}) {
            TypeId id = prog->lookupType(name);
            if (id != kNoType) (void)prog->layoutOf(id);
        }
        auto v = prog->lookupVariable("tab");
        if (v) (void)prog->evalInitializer(*v);
    }
    CHECK(true);
}

TEST_CASE("parser: declarations() lists named entities with their kinds") {
    auto prog = parseText(R"(
        namespace N { struct R {}; enum E { e1 }; typedef int I; constexpr int V = 1; template <typename T> struct TT {}; }
    )");
    Program& p = *prog;
    CHECK(hasDecl(p, "N", DeclInfo::Kind::Namespace));
    CHECK(hasDecl(p, "N::R", DeclInfo::Kind::Record));
    CHECK(hasDecl(p, "N::E", DeclInfo::Kind::Enum));
    CHECK(hasDecl(p, "N::I", DeclInfo::Kind::Typedef));
    CHECK(hasDecl(p, "N::V", DeclInfo::Kind::Variable));
    bool tmpl = false;
    for (const DeclInfo& d : p.declarations())
        if (d.qualifiedName == "N::TT") tmpl = d.isTemplate;
    CHECK(tmpl);
    CHECK(p.declarationCount() > 5);
}
