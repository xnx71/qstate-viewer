// Checks k12.hpp against the KT128 test vectors of RFC 9861 / the K12 reference (empty customization string).
#include "k12.hpp"
#include <cstdio>
#include <vector>
#include <string>

static std::string hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef"; std::string s;
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}
static std::vector<uint8_t> ptn(size_t n)
{
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = uint8_t(i % 251);
    return v;
}
int main()
{
    struct V { size_t n; const char* exp; } vs[] = {
        { 0,        "1ac2d450fc3b4205d19da7bfca1b37513c0803577ac7167f06fe2ce1f0ef39e5" },
        { 1,        "2bda92450e8b147f8a7cb629e784a058efca7cf7d8218e02d345dfaa65244a1f" },
        { 17,       "6bf75fa2239198db4772e36478f8e19b0f371205f6a9a93a273f51df37122888" },
        { 289,      "0c315ebcdedbf61426de7dcf8fb725d1e74675d7f5327a5067f367b108ecb67c" },
        { 4913,     "cb552e2ec77d9910701d578b457ddf772c12e322e4ee7fe417f92c758f0d59d0" },
        { 83521,    "8701045e22205345ff4dda05555cbb5c3af1a771c2b89baef37db43d9998b9fe" },
        { 1419857,  "844d610933b1b9963cbdeb5ae3b6b05cc7cbd67ceedf883eb678a0a8e0371682" },
        { 24137569, "3c390782a8a4e89fa6367f72feaaf13255c8d95878481d3cd8ce85f58e880af8" },
    };
    int bad = 0;
    for (auto& v : vs)
    {
        auto m = ptn(v.n);
        uint8_t out[32];
        k12::hash(m.data(), m.size(), out, 32);
        bool ok = hex(out, 32) == v.exp;
        std::printf("KT128(ptn(%zu)) = %s  %s\n", v.n, hex(out, 32).c_str(), ok ? "OK" : "MISMATCH");
        bad += !ok;
    }
    return bad;
}
