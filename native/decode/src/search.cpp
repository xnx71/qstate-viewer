// Byte pattern search over the whole state file.
#include <cctype>
#include <charconv>
#include <cstring>
#include <functional>

#include "impl.h"

namespace qstate::decode {

namespace {

struct Pattern {
    std::string mode;
    std::vector<std::uint8_t> bytes;
    std::string note;
    std::uint64_t align = 1;
};

bool isHexDigit(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    return std::tolower(static_cast<unsigned char>(c)) - 'a' + 10;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool allOf(const std::string& s, int (*pred)(int)) {
    if (s.empty()) return false;
    for (char c : s)
        if (!pred(static_cast<unsigned char>(c))) return false;
    return true;
}

bool parseHex(std::string s, std::vector<std::uint8_t>& out) {
    std::string digits;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ':') continue;
        if (!isHexDigit(c)) return false;
        digits.push_back(c);
    }
    if (digits.empty() || digits.size() % 2 != 0) return false;
    out.clear();
    for (std::size_t i = 0; i < digits.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(hexVal(digits[i]) * 16 + hexVal(digits[i + 1])));
    return true;
}

Pattern parsePattern(const SearchRequest& req, const IdentityCodec& codec) {
    Pattern p;
    const std::string q = trim(req.query);
    if (q.empty()) throw InvalidArgumentError("empty search query");
    std::string mode = req.mode.empty() ? "auto" : req.mode;
    if (mode == "auto") {
        auto upper = [](int c) { return std::isupper(c) ? 1 : 0; };
        auto digit = [](int c) { return std::isdigit(c) ? 1 : 0; };
        if (q.size() == 60 && allOf(q, upper)) mode = "id";
        else if (q.size() > 2 && q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) mode = "hex";
        else if (allOf(q, digit) || (q.size() > 1 && q[0] == '-' && allOf(q.substr(1), digit))) mode = "int";
        else if (q.size() % 2 == 0 && q.size() >= 4 && [&] {
                     for (char c : q)
                         if (!isHexDigit(c)) return false;
                     return true;
                 }())
            mode = "hex";
        else mode = "text";
    }
    p.mode = mode;
    if (mode == "id") {
        std::uint8_t key[32];
        std::string err;
        if (codec.decode(q, key, &err)) {
            p.note = "identity";
        } else {
            BasicIdentityCodec lax;
            std::string err2;
            if (!lax.decode(q, key, &err2)) throw InvalidArgumentError(err2);
            p.note = "identity (" + err + ")";
        }
        p.bytes.assign(key, key + 32);
        p.align = 8;
    } else if (mode == "hex") {
        if (!parseHex(q, p.bytes)) throw InvalidArgumentError("not a valid even-length hex string");
        p.note = "bytes";
    } else if (mode == "int") {
        std::uint64_t u = 0;
        bool neg = !q.empty() && q[0] == '-';
        const char* b = q.data() + (neg ? 1 : 0);
        auto r = std::from_chars(b, q.data() + q.size(), u);
        if (r.ec != std::errc() || r.ptr != q.data() + q.size() || (neg && u > (std::uint64_t{1} << 63)))
            throw InvalidArgumentError("integer out of the 64-bit range");
        if (neg) u = ~u + 1;
        for (int i = 0; i < 8; ++i) p.bytes.push_back(static_cast<std::uint8_t>((u >> (8 * i)) & 0xFF));
        p.note = "64-bit little-endian integer, 8-byte aligned";
        p.align = 8;
    } else if (mode == "text") {
        p.bytes.assign(q.begin(), q.end());
        p.note = "ASCII text";
    } else {
        throw InvalidArgumentError("unknown search mode: " + mode);
    }
    if (p.bytes.size() > 4096) throw InvalidArgumentError("pattern too long");
    return p;
}

} // namespace

SearchResult StateDecoder::Impl::search(const SearchRequest& req, const Query& q) const {
    const auto t0 = std::chrono::steady_clock::now();
    const Pattern pat = parsePattern(req, *codec);
    SearchResult res;
    res.mode = pat.mode;
    res.patternHex = fmt::hexBytes(pat.bytes.data(), pat.bytes.size());
    res.note = pat.note;
    const std::uint64_t limit = std::max<std::uint64_t>(1, std::min<std::uint64_t>(req.limit ? req.limit : 200, 5000));

    const std::uint64_t fileSize = src->size();
    const std::size_t plen = pat.bytes.size();
    constexpr std::size_t kBlock = 4u << 20;
    std::vector<std::uint8_t> buf(kBlock + plen);
    std::vector<std::uint64_t> hits;
    std::boyer_moore_horspool_searcher<const std::uint8_t*> searcher(pat.bytes.data(), pat.bytes.data() + plen);

    for (std::uint64_t pos = 0; pos < fileSize && hits.size() <= limit; pos += kBlock) {
        checkCancel(q);
        const std::size_t got = read(pos, kBlock + plen - 1, buf.data());
        if (got < plen) break;
        const std::uint8_t* const begin = buf.data();
        const std::uint8_t* const end = buf.data() + got;
        const std::uint64_t startLimit = std::min<std::uint64_t>(kBlock, got - plen + 1); // matches starting here
        if (pat.align > 1) {
            // aligned patterns (ids, integers): compare only at aligned offsets, first word first
            std::uint64_t first = 0;
            const std::size_t k = std::min<std::size_t>(plen, 8);
            std::memcpy(&first, pat.bytes.data(), k);
            for (std::uint64_t rel = 0; rel < startLimit; rel += pat.align) {
                std::uint64_t w = 0;
                std::memcpy(&w, begin + rel, k);
                if (w != first) continue;
                if (plen > 8 && std::memcmp(begin + rel + 8, pat.bytes.data() + 8, plen - 8) != 0) continue;
                hits.push_back(pos + rel);
                if (hits.size() > limit) break;
            }
            continue;
        }
        const std::uint8_t* it = begin;
        while (it < end) {
            const std::uint8_t* hit = std::search(it, end, searcher);
            if (hit == end) break;
            const std::uint64_t rel = static_cast<std::uint64_t>(hit - begin);
            if (rel >= startLimit) break;
            if ((pos + rel) % pat.align == 0) {
                hits.push_back(pos + rel);
                if (hits.size() > limit) break;
            }
            it = hit + 1;
        }
    }
    res.truncated = hits.size() > limit;
    if (res.truncated) hits.resize(static_cast<std::size_t>(limit));
    for (std::uint64_t off : hits) {
        SearchMatch m;
        m.offset = off;
        m.length = plen;
        try {
            m.location = locate(off, q);
        } catch (const NotFoundError&) {
            // match beyond the schema's state type (file larger than the type): no node, report the offset only
            m.location.id.clear();
            m.location.offset = off;
            m.location.size = plen;
            m.location.typeName = "(beyond the state type)";
        }
        res.matches.push_back(std::move(m));
    }
    res.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

} // namespace qstate::decode
