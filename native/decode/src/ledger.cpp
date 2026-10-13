// Ledger tables (spectrum.EEE / universe.EEE): the bulk scan that yields the live slots and the aggregates, the
// logical children of universe records, previews, ledger.summary and the hash lookup.
#include <cctype>

#include "impl.h"

namespace qstate::decode {

using Impl = StateDecoder::Impl;
using wide::i128;

namespace {

bool anyNonZero(const std::uint8_t* p, std::size_t n) {
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, p + i, 8);
        if (w) return true;
    }
    for (; i < n; ++i)
        if (p[i]) return true;
    return false;
}

std::int64_t s64At(const std::uint8_t* p) { return static_cast<std::int64_t>(fmt::loadLe(p, 8)); }

} // namespace

// ---- LedgerScan -----------------------------------------------------------------------------------------------------

const LedgerScan::Issuance* LedgerScan::issuance(std::uint64_t slot) const {
    auto it = std::lower_bound(issuances.begin(), issuances.end(), slot,
                               [](const Issuance& a, std::uint64_t s) { return a.slot < s; });
    return (it != issuances.end() && it->slot == slot) ? &*it : nullptr;
}

const LedgerScan::Ownership* LedgerScan::ownership(std::uint64_t slot) const {
    auto it = std::lower_bound(ownerships.begin(), ownerships.end(), slot,
                               [](const Ownership& a, std::uint64_t s) { return a.slot < s; });
    return (it != ownerships.end() && it->slot == slot) ? &*it : nullptr;
}

std::string LedgerScan::assetName(unsigned type, std::uint64_t idx) const {
    const Issuance* is = nullptr;
    if (type == 1) is = issuance(idx);
    else if (type == 2) is = issuance(idx);
    else if (type == 3) {
        const Ownership* o = ownership(idx);
        if (o) is = issuance(o->issuanceIndex);
    }
    return is ? std::string(is->name) : std::string();
}

std::size_t LedgerScan::bytes() const {
    std::size_t b = sizeof(LedgerScan) + live.capacity() * 4;
    for (const auto& v : byType) b += v.capacity() * 4;
    b += issuances.capacity() * sizeof(Issuance) + ownerships.capacity() * sizeof(Ownership) +
         possessions.capacity() * sizeof(Possession);
    return b;
}

// ---- record predicates ----------------------------------------------------------------------------------------------

bool Impl::ledgerRecordLive(const LedgerLayout& l, const std::uint8_t* rec, std::size_t avail) {
    if (avail < l.recordSize) return false;
    if (l.kind == schema::LedgerKind::Spectrum) return anyNonZero(rec + l.keyOff, 32);
    return rec[l.typeOff] != 0;
}

unsigned Impl::assetType(const LedgerLayout& l, const std::uint8_t* rec, std::size_t avail) {
    return avail > l.typeOff ? rec[l.typeOff] : 0u;
}

// ---- the bulk scan --------------------------------------------------------------------------------------------------

std::shared_ptr<const LedgerScan> Impl::ledgerScan(const Ref& c, const Query& q) const {
    const std::string k = key(q, "ls:" + std::to_string(c.offset) + ":" + std::to_string(c.type));
    if (auto hit = cache->getAs<LedgerScan>(k)) return hit;

    const TypeX& tx = idx.x(c.type);
    const LedgerLayout& l = tx.ledger;
    const std::uint64_t recSize = l.recordSize, count = tx.count;
    auto scan = std::make_shared<LedgerScan>();
    if (recSize == 0 || count == 0) return scan;

    constexpr std::uint64_t kChunkBytes = 1u << 20;
    const std::uint64_t perChunk = std::max<std::uint64_t>(1, kChunkBytes / recSize);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(std::min(perChunk, count) * recSize));
    const bool spectrum = l.kind == schema::LedgerKind::Spectrum;
    const std::uint64_t typeOff = l.typeOff;
    for (std::uint64_t i0 = 0; i0 < count; i0 += perChunk) {
        checkCancel(q);
        const std::uint64_t n = std::min(perChunk, count - i0);
        const std::size_t want = static_cast<std::size_t>(n * recSize);
        const std::size_t got = read(c.offset + i0 * recSize, want, buf.data());
        const std::uint64_t full = got / recSize;
        if (recSize % 8 == 0 && !anyNonZero(buf.data(), static_cast<std::size_t>(full * recSize))) {
            if (full < n) {
                scan->truncated = true;
                break;
            }
            continue;
        }
        for (std::uint64_t i = 0; i < full; ++i) {
            const std::uint8_t* p = buf.data() + i * recSize;
            const std::uint32_t slot = static_cast<std::uint32_t>(i0 + i);
            if (spectrum) {
                if (!anyNonZero(p + l.keyOff, 32)) continue;
                scan->live.push_back(slot);
                const i128 balance = static_cast<i128>(s64At(p + l.incomingOff)) - static_cast<i128>(s64At(p + l.outgoingOff));
                scan->totalSupply += balance;
                if (balance > 0) ++scan->nonZeroBalances;
                if (!scan->haveMax || balance > scan->maxBalance) {
                    scan->haveMax = true;
                    scan->maxBalance = balance;
                    scan->maxSlot = slot;
                }
                continue;
            }
            const unsigned type = p[typeOff];
            if (type == 0) continue;
            scan->live.push_back(slot);
            if (type < 1 || type > 3) {
                ++scan->otherTypes;
                continue;
            }
            scan->byType[type - 1].push_back(slot);
            const LedgerLayout::Alternative& a = l.alts[type - 1];
            if (type == 1) {
                LedgerScan::Issuance is;
                is.slot = slot;
                std::memcpy(is.key, p + l.keyOff, 32);
                std::memcpy(is.name, p + a.nameOff, 7);
                is.name[7] = 0;
                is.decimals = static_cast<std::int8_t>(p[a.decimalsOff]);
                std::memcpy(is.uom, p + a.uomOff, 7);
                scan->issuances.push_back(is);
            } else if (type == 2) {
                scan->ownerships.push_back({slot, static_cast<std::uint32_t>(fmt::loadLe(p + a.indexOff, 4)), s64At(p + a.sharesOff)});
            } else {
                scan->possessions.push_back({slot, static_cast<std::uint32_t>(fmt::loadLe(p + a.indexOff, 4))});
            }
        }
        if (full < n) {
            scan->truncated = true;
            break;
        }
    }
    // per-issuance aggregates (records may reference slots anywhere in the table: resolve after the pass)
    if (!spectrum) {
        for (const LedgerScan::Ownership& o : scan->ownerships) {
            auto it = std::lower_bound(scan->issuances.begin(), scan->issuances.end(), o.issuanceIndex,
                                       [](const LedgerScan::Issuance& a, std::uint64_t s) { return a.slot < s; });
            if (it != scan->issuances.end() && it->slot == o.issuanceIndex) {
                ++it->ownerships;
                it->totalShares += static_cast<i128>(o.shares);
            }
        }
        for (const LedgerScan::Possession& po : scan->possessions) {
            const LedgerScan::Ownership* o = scan->ownership(po.ownershipIndex);
            if (!o) continue;
            auto it = std::lower_bound(scan->issuances.begin(), scan->issuances.end(), o->issuanceIndex,
                                       [](const LedgerScan::Issuance& a, std::uint64_t s) { return a.slot < s; });
            if (it != scan->issuances.end() && it->slot == o->issuanceIndex) ++it->possessions;
        }
    }
    scan->live.shrink_to_fit();
    for (auto& v : scan->byType) v.shrink_to_fit();
    scan->issuances.shrink_to_fit();
    scan->ownerships.shrink_to_fit();
    scan->possessions.shrink_to_fit();
    cache->put(k, scan, scan->bytes());
    return scan;
}

// ---- universe record children ---------------------------------------------------------------------------------------

std::vector<Ref> Impl::assetLogicalChildren(const Ref& rec) const {
    const LedgerLayout& l = idx.x(rec.type).ledger;
    std::vector<Ref> out;
    std::uint8_t b = 0;
    const bool readable = read(rec.offset + l.typeOff, 1, &b) == 1;
    const unsigned type = readable ? b : 0u;
    const bool known = type >= 1 && type <= 3;
    const LedgerLayout::Alternative& a = l.alts[known ? type - 1 : 0];
    for (const Member& m : idx.x(a.type).members) {
        // an empty / unknown record shows only the common prefix (publicKey, type)
        if (!known && a.offset + m.offset + m.size > l.typeOff + 1) continue;
        Ref r = plainRef(rec, "a:" + m.name, m.name, m.type, a.offset + m.offset, m.size, m.assetNameHint);
        r.bitOffset = m.bitOffset;
        r.bitWidth = m.bitWidth;
        r.ledgerTypeHint = m.size == 1 && a.offset + m.offset == l.typeOff; // the `type` byte: enum-like value
        out.push_back(std::move(r));
    }
    return out;
}

Ref Impl::assetAltMember(const Ref& rec, std::string_view name) const {
    for (Ref& r : assetLogicalChildren(rec))
        if (r.label == name) return r;
    throw NotFoundError("no child 'a:" + std::string(name) + "' under node '" + rec.id + "'");
}

// ---- previews -------------------------------------------------------------------------------------------------------

std::string Impl::entityPreview(const std::uint8_t* rec, std::size_t avail) const {
    return fmt.entityPreview(rootLedger(), rec, avail);
}

std::string Impl::assetNameByReads(const Ref& table, unsigned type, std::uint64_t index) const {
    const TypeX& tx = idx.x(table.type);
    const LedgerLayout& l = tx.ledger;
    if (l.kind != schema::LedgerKind::Universe || index >= tx.count) return {};
    std::uint8_t buf[64];
    if (l.recordSize > sizeof buf) return {};
    auto readRecord = [&](std::uint64_t slot) {
        return readExact(table.offset + slot * l.recordSize, static_cast<std::size_t>(l.recordSize), buf);
    };
    if (type == 3) {
        if (!readRecord(index) || buf[l.typeOff] != 2) return {};
        index = fmt::loadLe(buf + l.alts[1].indexOff, 4);
        if (index >= tx.count) return {};
        type = 2;
    }
    if (type != 2 && type != 1) return {};
    if (!readRecord(index) || buf[l.typeOff] != 1) return {};
    return fmt::bytesText(buf + l.alts[0].nameOff, 7).value_or(std::string());
}

std::string Impl::assetPreview(const Ref& table, const std::uint8_t* rec, std::size_t avail) const {
    const LedgerLayout& l = idx.x(table.type).ledger;
    std::string name;
    const unsigned type = assetType(l, rec, avail);
    if ((type == 2 || type == 3) && avail >= l.recordSize)
        name = assetNameByReads(table, type, fmt::loadLe(rec + l.alts[type - 1].indexOff, 4));
    return fmt.assetPreview(l, rec, avail, name);
}

// ---- summary --------------------------------------------------------------------------------------------------------

LedgerSummary Impl::ledgerSummary(const Query& q) const {
    const auto t0 = std::chrono::steady_clock::now();
    if (!rootIsLedger()) throw InvalidArgumentError("the state is not a ledger table");
    const Ref root = rootRef();
    const TypeX& tx = idx.x(root.type);
    const LedgerLayout& l = tx.ledger;
    const auto scan = ledgerScan(root, q);
    LedgerSummary s;
    s.kind = l.kind;
    s.capacity = tx.count;
    s.population = scan->live.size();
    if (l.kind == schema::LedgerKind::Spectrum) {
        s.nonZeroBalances = scan->nonZeroBalances;
        s.totalSupply = Formatter::decimalI128(scan->totalSupply);
        s.maxSlot = scan->maxSlot;
        s.maxBalance = Formatter::decimalI128(scan->haveMax ? scan->maxBalance : i128(0));
        std::uint8_t key[32] = {};
        if (scan->haveMax) readExact(root.offset + std::uint64_t{scan->maxSlot} * l.recordSize + l.keyOff, 32, key);
        s.maxIdentity = codec->encode(key);
    } else {
        s.issuances = scan->byType[0].size();
        s.ownerships = scan->byType[1].size();
        s.possessions = scan->byType[2].size();
        s.assets.reserve(scan->issuances.size());
        for (const LedgerScan::Issuance& is : scan->issuances) {
            LedgerSummary::Asset a;
            a.slot = is.slot;
            a.issuer = codec->encode(is.key);
            a.name = is.name;
            a.numberOfDecimalPlaces = is.decimals;
            a.unitOfMeasurement = fmt::hexBytes(is.uom, 7);
            a.ownerships = is.ownerships;
            a.possessions = is.possessions;
            a.totalShares = Formatter::decimalI128(is.totalShares);
            s.assets.push_back(std::move(a));
        }
    }
    s.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return s;
}

// ---- lookup ---------------------------------------------------------------------------------------------------------

void Impl::parseKey(std::string_view text, std::uint8_t* key32) const {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (text.size() == 64) {
        for (int i = 0; i < 32; ++i) {
            const char a = text[static_cast<std::size_t>(2 * i)], b = text[static_cast<std::size_t>(2 * i + 1)];
            if (!std::isxdigit(static_cast<unsigned char>(a)) || !std::isxdigit(static_cast<unsigned char>(b)))
                throw InvalidArgumentError("not a hex public key: " + std::string(text));
            auto hv = [](char ch) { return ch <= '9' ? ch - '0' : std::tolower(static_cast<unsigned char>(ch)) - 'a' + 10; };
            key32[i] = static_cast<std::uint8_t>(hv(a) * 16 + hv(b));
        }
        return;
    }
    std::string err;
    if (!codec->decode(text, key32, &err))
        throw InvalidArgumentError(err.empty() ? "not an identity (60 letters) or a public key (64 hex digits)" : err);
}

LedgerLookup Impl::ledgerLookup(std::string_view identity, const Query& q) const {
    if (!rootIsLedger()) throw InvalidArgumentError("the state is not a ledger table");
    std::uint8_t key[32];
    parseKey(identity, key);
    const Ref root = rootRef();
    const TypeX& tx = idx.x(root.type);
    const LedgerLayout& l = tx.ledger;
    LedgerLookup out;
    out.identity = codec->encode(key);
    const std::uint64_t capacity = tx.count;
    const std::uint32_t home = static_cast<std::uint32_t>(fmt::loadLe(key, 4));
    const bool pow2 = (capacity & (capacity - 1)) == 0;
    std::uint64_t slot = pow2 ? (home & (capacity - 1)) : (home % capacity);
    std::vector<std::uint8_t> rec(static_cast<std::size_t>(l.recordSize));
    for (std::uint64_t n = 0; n < capacity; ++n) {
        if ((n & 0xFFF) == 0) checkCancel(q);
        ++out.probes;
        const std::size_t got = read(root.offset + slot * l.recordSize, rec.size(), rec.data());
        if (!ledgerRecordLive(l, rec.data(), got)) break; // an empty slot ends the probe sequence (also: end of file)
        if (std::memcmp(rec.data() + l.keyOff, key, 32) == 0) {
            LedgerLookupHit hit;
            hit.slot = slot;
            hit.id = joinId(root.id, "i:" + std::to_string(slot));
            if (l.kind == schema::LedgerKind::Universe) {
                hit.type = rec[l.typeOff];
                hit.preview = assetPreview(root, rec.data(), got);
            } else {
                hit.preview = entityPreview(rec.data(), got);
            }
            out.hits.push_back(std::move(hit));
            if (l.kind == schema::LedgerKind::Spectrum) break; // one record per key (the node stops here too)
        }
        slot = pow2 ? ((slot + 1) & (capacity - 1)) : ((slot + 1) % capacity);
    }
    return out;
}

// ---- public wrappers ------------------------------------------------------------------------------------------------

LedgerSummary StateDecoder::ledgerSummary(const Query& q) const {
    impl_->touch(q);
    return impl_->ledgerSummary(q);
}

LedgerLookup StateDecoder::ledgerLookup(std::string_view identity, const Query& q) const {
    impl_->touch(q);
    return impl_->ledgerLookup(identity, q);
}

std::optional<schema::LedgerKind> StateDecoder::ledgerKind() const {
    if (!impl_->rootIsLedger()) return std::nullopt;
    return impl_->rootLedger().kind;
}

} // namespace qstate::decode
