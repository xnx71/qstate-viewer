#include "qstate/support/tar.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

namespace qstate::support {

namespace fs = std::filesystem;

namespace {

bool parseOctal(const char* p, size_t n, uint64_t& out) {
    // GNU base-256 encoding for huge values.
    if (n > 0 && (static_cast<unsigned char>(p[0]) & 0x80)) {
        uint64_t v = static_cast<unsigned char>(p[0]) & 0x7F;
        for (size_t i = 1; i < n; i++) v = (v << 8) | static_cast<unsigned char>(p[i]);
        out = v;
        return true;
    }
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\0')) i++;
    bool any = false;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
        v = v * 8 + static_cast<uint64_t>(p[i] - '0');
        any = true;
    }
    for (; i < n; i++) {
        if (p[i] != ' ' && p[i] != '\0') return false;
    }
    if (!any && n > 0) v = 0;
    out = v;
    return true;
}

std::string cString(const char* p, size_t n) { return std::string(p, strnlen(p, n)); }

}  // namespace

TarExtractor::TarExtractor(std::string destDir) : dest_(std::move(destDir)) {
    std::error_code ec;
    fs::create_directories(dest_, ec);
    if (ec) fail("cannot create '" + dest_ + "': " + ec.message());
}

TarExtractor::~TarExtractor() { closeFile(); }

void TarExtractor::closeFile() {
    if (file_) {
        if (std::fclose(file_) != 0 && error_.empty()) error_ = "write error on '" + entryPath_ + "'";
        file_ = nullptr;
    }
}

void TarExtractor::fail(const std::string& message) {
    if (error_.empty()) error_ = message;
    closeFile();
}

bool TarExtractor::resolvePath(const std::string& nameIn, std::string& out) {
    std::string name = nameIn;
    while (!name.empty() && name.back() == '/') name.pop_back();
    if (name.empty() || name[0] == '/') return false;
    fs::path rel;
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('/', start);
        if (end == std::string::npos) end = name.size();
        const std::string part = name.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == ".." || part.find('\0') != std::string::npos) return false;
#if defined(_WIN32)
        if (part.find_first_of("\\:") != std::string::npos) return false;
#endif
        rel /= part;
    }
    if (rel.empty()) return false;
    out = (fs::path(dest_) / rel).string();
    return true;
}

void TarExtractor::parsePax(const std::string& records) {
    // "<len> <key>=<value>\n" records
    size_t pos = 0;
    while (pos < records.size()) {
        const size_t space = records.find(' ', pos);
        if (space == std::string::npos) break;
        const size_t len = static_cast<size_t>(std::strtoull(records.c_str() + pos, nullptr, 10));
        if (len == 0 || pos + len > records.size()) break;
        const std::string rec = records.substr(space + 1, pos + len - space - 2);  // without the trailing newline
        const size_t eq = rec.find('=');
        if (eq != std::string::npos && rec.substr(0, eq) == "path" && kind_ == Kind::PaxLocal) {
            pendingPath_ = rec.substr(eq + 1);
            hasPendingPath_ = true;
        }
        pos += len;
    }
}

void TarExtractor::processHeader() {
    bool zero = true;
    for (char c : header_) {
        if (c != 0) {
            zero = false;
            break;
        }
    }
    if (zero) {
        if (++zeroBlocks_ >= 2) state_ = State::End;
        return;
    }
    zeroBlocks_ = 0;

    uint64_t stored = 0;
    if (!parseOctal(header_ + 148, 8, stored)) return fail("corrupt tar header (checksum field)");
    uint64_t sum = 0;
    for (size_t i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? 0x20 : static_cast<unsigned char>(header_[i]);
    if (sum != stored) return fail("corrupt tar header (checksum mismatch)");

    uint64_t size = 0;
    if (!parseOctal(header_ + 124, 12, size)) return fail("corrupt tar header (size field)");
    const char type = header_[156];

    std::string name = cString(header_, 100);
    if (std::memcmp(header_ + 257, "ustar", 5) == 0) {
        const std::string prefix = cString(header_ + 345, 155);
        if (!prefix.empty()) name = prefix + "/" + name;
    }

    Kind kind;
    switch (type) {
        case '0':
        case '\0':
        case '7': kind = Kind::File; break;
        case '5': kind = Kind::Directory; break;
        case 'x': kind = Kind::PaxLocal; break;
        case 'g': kind = Kind::PaxGlobal; break;
        case 'L': kind = Kind::LongName; break;
        default: kind = Kind::Skip; break;
    }
    if (kind == Kind::File || kind == Kind::Directory) {
        if (hasPendingPath_) {
            name = pendingPath_;
            hasPendingPath_ = false;
        }
        pendingPath_.clear();
        std::string target;
        if (!resolvePath(name, target)) return fail("unsafe path in tar archive: '" + name + "'");
        entryPath_ = target;
    } else if (kind == Kind::Skip) {
        skipped_++;
        hasPendingPath_ = false;
    }
    beginEntry(kind, size);
}

void TarExtractor::beginEntry(Kind kind, uint64_t size) {
    kind_ = kind;
    remaining_ = size;
    padding_ = static_cast<size_t>((512 - size % 512) % 512);
    meta_.clear();
    if (kind == Kind::Directory) {
        std::error_code ec;
        fs::create_directories(entryPath_, ec);
        if (ec) return fail("cannot create directory '" + entryPath_ + "': " + ec.message());
        dirs_++;
    } else if (kind == Kind::File) {
        std::error_code ec;
        fs::create_directories(fs::path(entryPath_).parent_path(), ec);
        if (ec) return fail("cannot create directory for '" + entryPath_ + "': " + ec.message());
        file_ = std::fopen(entryPath_.c_str(), "wb");
        if (!file_) return fail("cannot create '" + entryPath_ + "'");
    }
    if (remaining_ == 0) {
        endEntry();
        return;
    }
    state_ = State::Data;
}

void TarExtractor::consumeData(const char* data, size_t size) {
    switch (kind_) {
        case Kind::File:
            if (std::fwrite(data, 1, size, file_) != size) return fail("write error on '" + entryPath_ + "'");
            bytes_ += size;
            break;
        case Kind::PaxLocal:
        case Kind::LongName:
            if (meta_.size() + size > (1u << 20)) return fail("oversized tar extended header");
            meta_.append(data, size);
            break;
        default: break;
    }
}

void TarExtractor::endEntry() {
    switch (kind_) {
        case Kind::File:
            closeFile();
            files_++;
            break;
        case Kind::PaxLocal: parsePax(meta_); break;
        case Kind::LongName:
            pendingPath_ = cString(meta_.data(), meta_.size());
            hasPendingPath_ = true;
            break;
        default: break;
    }
    state_ = padding_ > 0 ? State::Padding : State::Header;
    headerFill_ = 0;
}

bool TarExtractor::feed(const char* data, size_t size) {
    while (size > 0 && error_.empty() && state_ != State::End) {
        switch (state_) {
            case State::Header: {
                const size_t n = std::min(size, sizeof(header_) - headerFill_);
                std::memcpy(header_ + headerFill_, data, n);
                headerFill_ += n;
                data += n;
                size -= n;
                if (headerFill_ == sizeof(header_)) {
                    headerFill_ = 0;
                    processHeader();
                }
                break;
            }
            case State::Data: {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(size, remaining_));
                consumeData(data, n);
                data += n;
                size -= n;
                remaining_ -= n;
                if (error_.empty() && remaining_ == 0) endEntry();
                break;
            }
            case State::Padding: {
                const size_t n = std::min(size, padding_);
                data += n;
                size -= n;
                padding_ -= n;
                if (padding_ == 0) state_ = State::Header;
                break;
            }
            case State::End: break;
        }
    }
    return error_.empty();
}

bool TarExtractor::finish() {
    finished_ = true;
    if (error_.empty() && (state_ == State::Data || state_ == State::Padding || headerFill_ != 0)) {
        fail("tar stream ended inside an entry");
    }
    closeFile();
    return error_.empty();
}

}  // namespace qstate::support
