// Byte access seam of the decoder: everything read from a state file goes through a ByteSource.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qstate::decode {

// Random access, read-only view of one state file (or any byte image).
// Thread-safety: implementations must allow concurrent const calls from several threads.
class ByteSource {
public:
    virtual ~ByteSource() = default;

    // Current size in bytes (may be smaller or larger than the schema's sizeof(state)).
    virtual std::uint64_t size() const = 0;

    // Reads up to `length` bytes starting at `offset` into `out`; returns the number of bytes actually read
    // (0 .. length). A short count means end of data (or an I/O failure); it never throws for out-of-range requests.
    virtual std::size_t read(std::uint64_t offset, std::size_t length, std::uint8_t* out) const = 0;

    // True when [offset, offset + length) lies inside the data and every byte is zero. The default implementation
    // reads in chunks; sparse / mapped sources may override it with something faster.
    virtual bool isAllZero(std::uint64_t offset, std::uint64_t length) const;
};

// In-memory image (tests, small files).
class MemoryByteSource final : public ByteSource {
public:
    MemoryByteSource() = default;
    explicit MemoryByteSource(std::vector<std::uint8_t> bytes) : data_(std::move(bytes)) {}

    std::uint64_t size() const override { return data_.size(); }
    std::size_t read(std::uint64_t offset, std::size_t length, std::uint8_t* out) const override;
    bool isAllZero(std::uint64_t offset, std::uint64_t length) const override;

    const std::vector<std::uint8_t>& bytes() const { return data_; }

private:
    std::vector<std::uint8_t> data_;
};

} // namespace qstate::decode
