// Internal: value formatting (leaf values, one-line previews) from raw bytes.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "int128.h"
#include "qstate/decode/identity.h"
#include "qstate/decode/schema_index.h"
#include "qstate/decode/values.h"

namespace qstate::decode {

using ContractNameFn = std::function<std::string(std::uint32_t)>;

namespace fmt {

std::uint64_t loadLe(const std::uint8_t* p, std::size_t n); // n <= 8
std::string hexBytes(const std::uint8_t* p, std::size_t n);
std::string hexNumber(std::uint64_t v, std::uint32_t bits); // "0x" + zero padded
std::string decimal128(std::uint64_t hi, std::uint64_t lo);
std::string dateTimeText(std::uint64_t v, bool& valid);
std::optional<std::string> assetNameText(std::uint64_t v);       // conservative: [A-Z0-9]{1,7}, rest zero
std::optional<std::uint64_t> assetNameValue(const std::string&); // inverse (text -> uint64)
std::optional<std::string> bytesText(const std::uint8_t* p, std::size_t n); // printable ASCII up to first NUL
std::string quoteText(const std::string& s);

} // namespace fmt

class Formatter {
public:
    Formatter(const SchemaIndex& idx, const IdentityCodec& codec, ContractNameFn names)
        : idx_(idx), codec_(codec), names_(std::move(names)) {}

    // Leaf-like type (Int, Bool, Char, Enum, Float, Ptr, Id, Bit, U128, DateTime, Opaque). `avail` = readable bytes at p.
    LeafValue leaf(TypeId t, const std::uint8_t* p, std::size_t avail, bool assetHint = false, std::uint32_t bitOffset = 0,
                   std::uint32_t bitWidth = 0) const;
    LeafValue idValue(const std::uint8_t* p32) const;
    LeafValue bytesValue(const std::uint8_t* p, std::size_t n) const; // hex capped at 64 bytes
    LeafValue bitsValue(const std::uint8_t* p, std::size_t nbytes, std::uint64_t bitCount) const;

    // One-line rendering of a leaf value.
    std::string shortText(const LeafValue& v) const;
    // One-line preview of any value of type `t` stored at p (only `avail` bytes readable); budget limits recursion.
    std::string preview(TypeId t, const std::uint8_t* p, std::size_t avail, bool assetHint = false, int depth = 0) const;

    // Slot summary of a ProposalVoting slot record (ProposalWithAllVoteData<...>): epoch, type, option histogram.
    std::optional<std::string> proposalSummary(TypeId t, const std::uint8_t* p, std::size_t avail) const;

    // Ledger records. `assetName` (universe ownership / possession) is the resolved name of the referenced issuance
    // when the caller knows it ("" otherwise: the slot index is shown instead).
    std::string entityPreview(const LedgerLayout& l, const std::uint8_t* p, std::size_t avail) const;
    std::string assetPreview(const LedgerLayout& l, const std::uint8_t* p, std::size_t avail,
                             const std::string& assetName = {}) const;
    // The `type` byte of a universe record as an enum-like value (1 issuance, 2 ownership, 3 possession, 0 empty).
    LeafValue assetTypeValue(unsigned type) const;
    static const char* assetTypeName(unsigned type); // "issuance" | "ownership" | "possession" | "empty" | nullptr
    // Decimal text of a signed 128-bit value.
    static std::string decimalI128(wide::i128 v);

    // Contract id recognition for an id value.
    std::optional<std::uint32_t> contractIndexOf(const std::uint8_t* p32) const;

    const SchemaIndex& index() const { return idx_; }
    const IdentityCodec& codec() const { return codec_; }
    std::string contractName(std::uint32_t index) const { return names_ ? names_(index) : std::string(); }

private:
    const SchemaIndex& idx_;
    const IdentityCodec& codec_;
    ContractNameFn names_;
};

} // namespace qstate::decode
