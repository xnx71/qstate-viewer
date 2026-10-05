#include "format.h"

#include "int128.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace qstate::decode {

LeafValue LeafValue::unavailable(std::string reason) {
    LeafValue v;
    v.k = Kind::Unavailable;
    v.v = std::move(reason);
    return v;
}

LeafValue LeafValue::composite(std::string preview) {
    LeafValue v;
    v.k = Kind::Composite;
    v.v = std::move(preview);
    return v;
}

const char* kindName(LeafValue::Kind kind) {
    using K = LeafValue::Kind;
    switch (kind) {
    case K::Int: return "int";
    case K::Bool: return "bool";
    case K::Char: return "char";
    case K::Enum: return "enum";
    case K::Id: return "id";
    case K::U128: return "u128";
    case K::Float: return "float";
    case K::DateTime: return "datetime";
    case K::Bits: return "bits";
    case K::Bytes: return "bytes";
    case K::Ptr: return "ptr";
    case K::Unavailable: return "unavailable";
    case K::Composite: return "composite";
    }
    return "unavailable";
}

const char* kindName(NodeKind kind) {
    switch (kind) {
    case NodeKind::Struct: return "struct";
    case NodeKind::Union: return "union";
    case NodeKind::Array: return "array";
    case NodeKind::BitArray: return "bitArray";
    case NodeKind::HashMap: return "hashMap";
    case NodeKind::HashSet: return "hashSet";
    case NodeKind::Collection: return "collection";
    case NodeKind::LinkedList: return "linkedList";
    case NodeKind::Entry: return "entry";
    case NodeKind::Pov: return "pov";
    case NodeKind::Leaf: return "leaf";
    }
    return "leaf";
}

namespace fmt {

std::uint64_t loadLe(const std::uint8_t* p, std::size_t n) {
    std::uint64_t v = 0;
    for (std::size_t i = n; i > 0; --i) v = (v << 8) | p[i - 1];
    return v;
}

std::string hexBytes(const std::uint8_t* p, std::size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(digits[p[i] >> 4]);
        s.push_back(digits[p[i] & 15]);
    }
    return s;
}

std::string hexNumber(std::uint64_t v, std::uint32_t bits) {
    static const char* digits = "0123456789abcdef";
    const std::uint32_t nd = std::max<std::uint32_t>(1, (bits + 3) / 4);
    std::string s(nd, '0');
    for (std::uint32_t i = 0; i < nd && i < 16; ++i) s[nd - 1 - i] = digits[(v >> (4 * i)) & 15];
    return "0x" + s;
}

std::string decimal128(std::uint64_t hi, std::uint64_t lo) {
    using u128 = wide::u128;
    u128 v = (static_cast<u128>(hi) << 64) | lo;
    if (v == 0) return "0";
    std::string s;
    while (v != 0) {
        s.push_back(static_cast<char>('0' + static_cast<int>(v % 10)));
        v /= 10;
    }
    std::reverse(s.begin(), s.end());
    return s;
}

namespace {
bool leapYear(std::uint64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
std::uint64_t daysInMonth(std::uint64_t y, std::uint64_t m) {
    static const std::uint64_t d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && leapYear(y)) ? 29 : d[m - 1];
}
} // namespace

std::string dateTimeText(std::uint64_t v, bool& valid) {
    if (v == 0) {
        valid = false;
        return "unset";
    }
    const std::uint64_t year = (v >> 46) & 0xFFFF, month = (v >> 42) & 0xF, day = (v >> 37) & 0x1F,
                        hour = (v >> 32) & 0x1F, minute = (v >> 26) & 0x3F, second = (v >> 20) & 0x3F,
                        ms = (v >> 10) & 0x3FF, us = v & 0x3FF;
    valid = month >= 1 && month <= 12 && day >= 1 && day <= daysInMonth(year, month) && hour < 24 && minute < 60 &&
            second < 60 && ms < 1000 && us < 1000 && (v >> 62) == 0;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04llu-%02llu-%02llu %02llu:%02llu:%02llu.%03llu'%03llu",
                  static_cast<unsigned long long>(year), static_cast<unsigned long long>(month),
                  static_cast<unsigned long long>(day), static_cast<unsigned long long>(hour),
                  static_cast<unsigned long long>(minute), static_cast<unsigned long long>(second),
                  static_cast<unsigned long long>(ms), static_cast<unsigned long long>(us));
    return buf;
}

std::optional<std::string> assetNameText(std::uint64_t v) {
    if (v == 0 || (v >> 56) != 0) return std::nullopt;
    std::string s;
    bool ended = false;
    for (int i = 0; i < 8; ++i) {
        const unsigned c = static_cast<unsigned>((v >> (8 * i)) & 0xFF);
        if (c == 0) {
            ended = true;
            continue;
        }
        if (ended) return std::nullopt; // non-zero byte after NUL
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return std::nullopt;
        s.push_back(static_cast<char>(c));
    }
    if (s.empty() || s.size() > 7) return std::nullopt;
    return s;
}

std::optional<std::uint64_t> assetNameValue(const std::string& s) {
    if (s.empty() || s.size() > 7) return std::nullopt;
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c > 0x7e) return std::nullopt;
        v |= static_cast<std::uint64_t>(c) << (8 * i);
    }
    return v;
}

std::optional<std::string> bytesText(const std::uint8_t* p, std::size_t n) {
    std::size_t len = 0;
    while (len < n && p[len] != 0) ++len;
    if (len == 0) return std::nullopt;
    for (std::size_t i = 0; i < len; ++i)
        if (p[i] < 0x20 || p[i] > 0x7e) return std::nullopt;
    for (std::size_t i = len; i < n; ++i)
        if (p[i] != 0) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(p), len);
}

std::string quoteText(const std::string& s) { return "\"" + s + "\""; }

} // namespace fmt

namespace {

std::string floatText(double d) {
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, d);
    if (r.ec != std::errc()) return "nan";
    return std::string(buf, r.ptr);
}

std::string charText(unsigned c) {
    if (c >= 0x20 && c <= 0x7e) return std::string(1, static_cast<char>(c));
    if (c == 0) return "\\0";
    char buf[16];
    std::snprintf(buf, sizeof buf, "\\x%02x", c & 0xFFu);
    return buf;
}

} // namespace

LeafValue Formatter::idValue(const std::uint8_t* p) const {
    LeafValue v;
    v.k = LeafValue::Kind::Id;
    v.identity = codec_.encode(p);
    v.hex = fmt::hexBytes(p, 32);
    bool allZero = true;
    for (int i = 0; i < 32; ++i)
        if (p[i]) {
            allZero = false;
            break;
        }
    v.zero = allZero;
    if (auto c = contractIndexOf(p)) {
        v.contractIndex = *c;
        v.contractName = contractName(*c);
    }
    if (!allZero) {
        if (auto t = fmt::bytesText(p, 32); t && t->size() >= 3) v.text = *t;
    }
    return v;
}

std::optional<std::uint32_t> Formatter::contractIndexOf(const std::uint8_t* p) const {
    const std::uint64_t w0 = fmt::loadLe(p, 8);
    if (w0 == 0 || w0 >= 1024) return std::nullopt;
    for (int i = 8; i < 32; ++i)
        if (p[i]) return std::nullopt;
    return static_cast<std::uint32_t>(w0);
}

LeafValue Formatter::bytesValue(const std::uint8_t* p, std::size_t n) const {
    LeafValue v;
    v.k = LeafValue::Kind::Bytes;
    v.length = n;
    const std::size_t shown = std::min<std::size_t>(n, 64);
    v.hex = fmt::hexBytes(p, shown);
    v.truncated = shown < n;
    if (auto t = fmt::bytesText(p, n)) v.text = *t;
    return v;
}

LeafValue Formatter::bitsValue(const std::uint8_t* p, std::size_t nbytes, std::uint64_t bitCount) const {
    LeafValue v;
    v.k = LeafValue::Kind::Bits;
    v.count = bitCount;
    std::uint64_t set = 0;
    for (std::size_t i = 0; i < nbytes; ++i) {
        std::uint8_t b = p[i];
        const std::uint64_t firstBit = static_cast<std::uint64_t>(i) * 8;
        if (firstBit + 8 > bitCount) {
            const std::uint64_t keep = bitCount > firstBit ? bitCount - firstBit : 0;
            b = static_cast<std::uint8_t>(b & ((1u << keep) - 1u));
        }
        set += static_cast<std::uint64_t>(std::popcount(static_cast<unsigned>(b)));
    }
    v.set = set;
    const std::size_t shown = std::min<std::size_t>(nbytes, 64);
    v.hex = fmt::hexBytes(p, shown);
    v.truncated = shown < nbytes;
    return v;
}

LeafValue Formatter::leaf(TypeId t, const std::uint8_t* p, std::size_t avail, bool assetHint, std::uint32_t bitOffset,
                          std::uint32_t bitWidth) const {
    if (!idx_.valid(t)) return LeafValue::unavailable("unknown type");
    const TypeX& tx = idx_.x(t);
    if (tx.size > avail) return LeafValue::unavailable("beyond end of file");
    LeafValue v;
    switch (tx.cls) {
    case Cls::Int:
    case Cls::Enum:
    case Cls::Char: {
        const std::size_t sz = static_cast<std::size_t>(tx.size);
        std::uint64_t raw = fmt::loadLe(p, sz);
        std::uint32_t bits = tx.intBits;
        if (bitWidth > 0 && bitWidth <= 64 && bitOffset < 64) {
            raw >>= bitOffset;
            bits = bitWidth;
            if (bits < 64) raw &= (std::uint64_t{1} << bits) - 1;
        }
        const std::uint64_t mask = bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
        raw &= mask;
        if (tx.cls == Cls::Char) {
            v.k = LeafValue::Kind::Char;
            v.v = charText(static_cast<unsigned>(raw));
            v.raw = static_cast<std::uint32_t>(raw);
            return v;
        }
        std::string dec;
        if (tx.intSigned) {
            std::int64_t sv = static_cast<std::int64_t>(raw);
            if (bits < 64 && ((raw >> (bits - 1)) & 1)) sv = static_cast<std::int64_t>(raw | ~mask);
            dec = std::to_string(sv);
        } else {
            dec = std::to_string(raw);
        }
        if (tx.cls == Cls::Enum) {
            v.k = LeafValue::Kind::Enum;
            v.v = dec;
            for (const schema::Enumerator& e : idx_.type(t).enumerators) {
                const std::uint64_t ev = static_cast<std::uint64_t>(e.value) & mask;
                if (ev == raw) {
                    v.name = e.name;
                    break;
                }
            }
            return v;
        }
        v.k = LeafValue::Kind::Int;
        v.v = dec;
        v.isUnsigned = !tx.intSigned;
        v.bits = bits;
        v.hex = fmt::hexNumber(raw, bits);
        if (assetHint && bits == 64 && !tx.intSigned)
            if (auto s = fmt::assetNameText(raw)) v.text = *s;
        return v;
    }
    case Cls::Bool:
    case Cls::Bit:
        v.k = LeafValue::Kind::Bool;
        v.raw = p[0];
        v.boolean = p[0] != 0;
        return v;
    case Cls::Float: {
        v.k = LeafValue::Kind::Float;
        if (tx.size == 4) {
            std::uint32_t u = static_cast<std::uint32_t>(fmt::loadLe(p, 4));
            float f;
            std::memcpy(&f, &u, 4);
            v.v = floatText(static_cast<double>(f));
        } else {
            std::uint64_t u = fmt::loadLe(p, 8);
            double d;
            std::memcpy(&d, &u, 8);
            v.v = floatText(d);
        }
        return v;
    }
    case Cls::Ptr:
        v.k = LeafValue::Kind::Ptr;
        v.hex = fmt::hexNumber(fmt::loadLe(p, 8), 64);
        return v;
    case Cls::Id: return idValue(p);
    case Cls::U128: {
        v.k = LeafValue::Kind::U128;
        const std::uint64_t lo = fmt::loadLe(p, 8), hi = fmt::loadLe(p + 8, 8);
        v.v = fmt::decimal128(hi, lo);
        v.hex = "0x" + fmt::hexNumber(hi, 64).substr(2) + fmt::hexNumber(lo, 64).substr(2);
        return v;
    }
    case Cls::DateTime: {
        v.k = LeafValue::Kind::DateTime;
        const std::uint64_t raw = fmt::loadLe(p, 8);
        bool valid = false;
        v.text = fmt::dateTimeText(raw, valid);
        v.valid = valid;
        v.rawDecimal = std::to_string(raw);
        return v;
    }
    default: return bytesValue(p, static_cast<std::size_t>(tx.size));
    }
}

std::string Formatter::shortText(const LeafValue& v) const {
    using K = LeafValue::Kind;
    switch (v.k) {
    case K::Int: return v.text ? v.v + " (" + *v.text + ")" : v.v;
    case K::Bool: return v.boolean ? "true" : "false";
    case K::Char: return "'" + v.v + "'";
    case K::Enum: return v.name ? *v.name + " (" + v.v + ")" : v.v;
    case K::Id: {
        if (v.contractIndex) {
            std::string s = "contract " + std::to_string(*v.contractIndex);
            if (!v.contractName.empty()) s += " (" + v.contractName + ")";
            return s;
        }
        if (v.zero) return "NULL_ID";
        if (v.text) return fmt::quoteText(*v.text);
        if (v.identity.size() == 60) return v.identity.substr(0, 8) + "\xE2\x80\xA6" + v.identity.substr(52);
        return v.identity;
    }
    case K::U128:
    case K::Float: return v.v;
    case K::DateTime: return v.text.value_or("");
    case K::Bits: return std::to_string(v.count) + " bits, " + std::to_string(v.set) + " set";
    case K::Bytes: {
        if (v.text) return fmt::quoteText(*v.text);
        const std::size_t shown = std::min<std::size_t>(v.hex.size(), 16);
        return v.hex.substr(0, shown) + ((v.truncated || shown < v.hex.size()) ? "\xE2\x80\xA6" : "");
    }
    case K::Ptr: return v.hex;
    case K::Unavailable: return "n/a";
    case K::Composite: return v.v;
    }
    return {};
}

namespace {

const char* proposalTypeName(unsigned type) {
    switch (type) {
    case 0x0002: return "YesNo";
    case 0x0003: return "ThreeOptions";
    case 0x0004: return "FourOptions";
    case 0x0102: return "TransferYesNo";
    case 0x0103: return "TransferTwoAmounts";
    case 0x0104: return "TransferThreeAmounts";
    case 0x0105: return "TransferFourAmounts";
    case 0x0402: return "TransferInEpochYesNo";
    case 0x0202: return "VariableYesNo";
    case 0x0203: return "VariableTwoValues";
    case 0x0204: return "VariableThreeValues";
    case 0x0205: return "VariableFourValues";
    case 0x0200: return "VariableScalarMean";
    case 0x0302: return "MultiVariablesYesNo";
    case 0x0303: return "MultiVariablesThreeOptions";
    case 0x0304: return "MultiVariablesFourOptions";
    default: return nullptr;
    }
}

} // namespace

// Decoded summary of one slot of a ProposalVoting (ProposalWithAllVoteData<ProposalData..., votes>): slot occupancy,
// type, option count and the vote histogram. Returns nullopt when `t` is not such a record.
std::optional<std::string> Formatter::proposalSummary(TypeId t, const std::uint8_t* p, std::size_t avail) const {
    const schema::Type& st = idx_.type(t);
    if (st.name.rfind("ProposalWithAllVoteData<", 0) != 0) return std::nullopt;
    const TypeX& tx = idx_.x(t);
    const Member *epoch = nullptr, *type = nullptr, *tick = nullptr, *votes = nullptr;
    for (const Member& m : tx.members) {
        if (m.name == "epoch") epoch = &m;
        else if (m.name == "type") type = &m;
        else if (m.name == "tick") tick = &m;
        else if (m.name == "votes") votes = &m;
    }
    if (!epoch || !type || !tick || !votes || epoch->size != 2 || type->size != 2 || tick->size != 4) return std::nullopt;
    if (epoch->offset + 2 > avail || type->offset + 2 > avail || tick->offset + 4 > avail) return std::nullopt;
    const unsigned ep = static_cast<unsigned>(fmt::loadLe(p + epoch->offset, 2));
    if (ep == 0) return std::string("free slot");
    const unsigned ty = static_cast<unsigned>(fmt::loadLe(p + type->offset, 2));
    std::string s = "epoch " + std::to_string(ep) + ", type 0x" + fmt::hexNumber(ty, 16).substr(2);
    if (const char* n = proposalTypeName(ty)) s += std::string(" ") + n;
    const unsigned options = ty & 0xFF;
    s += options ? ", " + std::to_string(options) + " options" : ", scalar";
    s += ", tick " + std::to_string(fmt::loadLe(p + tick->offset, 4));
    // votes
    if (votes->offset + votes->size <= avail && options > 0 && options <= 8) {
        bool yesNo = false;
        for (const schema::BaseClass& b : st.bases)
            if (b.typeName.find("YesNo") != std::string::npos) yesNo = true;
        std::uint64_t hist[8] = {};
        std::uint64_t casted = 0, slots = 0;
        const std::uint8_t* v = p + votes->offset;
        const std::uint64_t votesSize = votes->size;
        const TypeX* vt = idx_.valid(votes->type) ? &idx_.x(votes->type) : nullptr;
        const std::uint64_t elemSize = vt && vt->cls == Cls::CArray ? vt->elemSize : 1;
        if (yesNo) {
            slots = votesSize * 4 > 676 ? 676 : votesSize * 4;
            for (std::uint64_t i = 0; i < slots; ++i) {
                const unsigned x = (v[i >> 2] >> ((i & 3) * 2)) & 3u;
                if (x < options && x < 3) {
                    ++hist[x];
                    ++casted;
                }
            }
        } else if (elemSize == 8) {
            slots = votesSize / 8;
            for (std::uint64_t i = 0; i < slots; ++i) {
                const std::uint64_t x = fmt::loadLe(v + 8 * i, 8);
                if (x < options) {
                    ++hist[x];
                    ++casted;
                }
            }
        } else {
            slots = votesSize;
            for (std::uint64_t i = 0; i < slots; ++i)
                if (v[i] < options) {
                    ++hist[v[i]];
                    ++casted;
                }
        }
        s += ", votes " + std::to_string(casted) + "/" + std::to_string(slots) + " [";
        for (unsigned k = 0; k < options && k < 8; ++k) s += (k ? ", " : "") + std::to_string(hist[k]);
        s += "]";
    }
    return s;
}

std::string Formatter::preview(TypeId t, const std::uint8_t* p, std::size_t avail, bool assetHint, int depth) const {
    if (!idx_.valid(t)) return "?";
    const TypeX& tx = idx_.x(t);
    const std::string ell = "\xE2\x80\xA6";
    switch (tx.cls) {
    case Cls::Record:
    case Cls::Union: {
        if (auto ps = proposalSummary(t, p, avail)) return *ps;
        if (depth >= 2) return "{" + ell + "}";
        std::string s = "{";
        std::size_t shown = 0;
        for (const Member& m : tx.members) {
            if (shown >= 5 || s.size() > 100) {
                s += ", " + ell;
                break;
            }
            if (m.offset + m.size > avail) {
                if (shown) s += ", ";
                s += ell;
                break;
            }
            if (shown) s += ", ";
            s += m.name + ": " + preview(m.type, p + m.offset, avail - static_cast<std::size_t>(m.offset),
                                         m.assetNameHint, depth + 1);
            ++shown;
        }
        return s + "}";
    }
    case Cls::CArray:
    case Cls::ArrayRole: {
        if (tx.byteElems) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(tx.count, avail));
            return shortText(bytesValue(p, n));
        }
        std::string s = "[";
        const std::uint64_t shown = std::min<std::uint64_t>(tx.count, 5);
        for (std::uint64_t i = 0; i < shown; ++i) {
            const std::uint64_t off = i * tx.elemSize;
            if (off + tx.elemSize > avail) {
                s += (i ? ", " : "") + ell;
                break;
            }
            if (i) s += ", ";
            s += preview(tx.elem, p + off, avail - static_cast<std::size_t>(off), false, depth + 1);
        }
        if (tx.count > shown) s += ", " + ell;
        return s + "] (" + std::to_string(tx.count) + ")";
    }
    case Cls::BitArray: {
        const std::size_t nb = static_cast<std::size_t>(std::min<std::uint64_t>((tx.count + 7) / 8, avail));
        return shortText(bitsValue(p, nb, tx.count));
    }
    case Cls::HashMap: return "HashMap, capacity " + std::to_string(tx.hash.capacity);
    case Cls::HashSet: return "HashSet, capacity " + std::to_string(tx.hash.capacity);
    case Cls::Collection: return "Collection, capacity " + std::to_string(tx.coll.capacity);
    case Cls::LinkedList: return "LinkedList, capacity " + std::to_string(tx.list.capacity);
    default: return shortText(leaf(t, p, avail, assetHint));
    }
}

} // namespace qstate::decode
