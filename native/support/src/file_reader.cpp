#include "qstate/support/file_reader.h"

#include <algorithm>
#include <cstring>

#include "platform.h"

namespace qstate::support {

namespace {

constexpr size_t kScanBlock = 4 * 1024 * 1024;
constexpr size_t kZeroBlock = 1024 * 1024;

FileStamp toStamp(const platform::FileStat& st) {
    FileStamp s;
    s.size = st.size;
    s.mtimeNs = st.mtimeNs;
    s.inode = st.inode;
    s.device = st.device;
    return s;
}

// True when all n bytes are zero.
bool allZero(const uint8_t* p, size_t n) {
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4];
        std::memcpy(w, p + i, 32);
        if ((w[0] | w[1] | w[2] | w[3]) != 0) return false;
    }
    for (; i < n; i++) {
        if (p[i] != 0) return false;
    }
    return true;
}

}  // namespace

struct FileReader::Handle {
    std::shared_ptr<platform::NativeFile> file;
    FileStamp stamp;
};

std::shared_ptr<const FileReader::Handle> FileReader::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handle_;
}

FileReader::FileReader(std::string path) : path_(std::move(path)) {
    int error = 0;
    auto file = platform::NativeFile::open(path_, &error);
    if (!file) throw FileError("cannot open '" + path_ + "': " + platform::errorMessage(error), error);
    auto handle = std::make_shared<Handle>();
    handle->stamp = toStamp(file->stat());
    handle->file = std::move(file);
    handle_ = std::move(handle);
}

std::unique_ptr<FileReader> FileReader::tryOpen(const std::string& path, std::string* error) {
    try {
        return std::make_unique<FileReader>(path);
    } catch (const FileError& e) {
        if (error) *error = e.what();
        return nullptr;
    }
}

FileReader::~FileReader() = default;

uint64_t FileReader::size() const { return current()->stamp.size; }
int64_t FileReader::mtimeNs() const { return current()->stamp.mtimeNs; }
FileStamp FileReader::stamp() const { return current()->stamp; }

size_t FileReader::read(uint64_t offset, size_t length, void* buffer) const {
    if (length == 0) return 0;
    const auto h = current();
    const auto r = h->file->readAt(offset, buffer, length);
    if (r.error != 0) {
        throw FileError("read error in '" + path_ + "' at offset " + std::to_string(offset) + ": " +
                            platform::errorMessage(r.error),
                        r.error);
    }
    return r.bytes;
}

std::vector<uint8_t> FileReader::readBytes(uint64_t offset, size_t length) const {
    std::vector<uint8_t> v(length);
    v.resize(read(offset, length, v.data()));
    return v;
}

std::string FileReader::readString(uint64_t offset, size_t length) const {
    std::string s(length, '\0');
    s.resize(read(offset, length, s.data()));
    return s;
}

RefreshResult FileReader::refresh() {
    const platform::FileStat onDisk = platform::statPath(path_);
    if (!onDisk.exists || onDisk.isDir) return RefreshResult::Missing;
    const auto h = current();
    const FileStamp pathStamp = toStamp(onDisk);
    const bool sameFile = pathStamp.inode == h->stamp.inode && pathStamp.device == h->stamp.device;
    auto next = std::make_shared<Handle>();
    if (sameFile) {
        // Same inode: the open handle sees the live size / mtime.
        next->file = h->file;
        next->stamp = toStamp(h->file->stat());
    } else {
        int error = 0;
        auto file = platform::NativeFile::open(path_, &error);
        if (!file) return RefreshResult::Missing;
        next->stamp = toStamp(file->stat());
        next->file = std::move(file);
    }
    const bool changed = !(next->stamp == h->stamp);
    if (changed) {
        std::lock_guard<std::mutex> lock(mutex_);
        handle_ = std::move(next);
    }
    return changed ? RefreshResult::Changed : RefreshResult::Unchanged;
}

std::optional<uint64_t> FileReader::firstNonZero(uint64_t offset, uint64_t length) const {
    std::vector<uint8_t> buf(kZeroBlock);
    uint64_t done = 0;
    while (done < length) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), length - done));
        const size_t got = read(offset + done, want, buf.data());
        for (size_t i = 0; i < got; i += 4096) {
            const size_t n = std::min<size_t>(4096, got - i);
            if (!allZero(buf.data() + i, n)) {
                size_t j = i;
                while (buf[j] == 0) j++;
                return offset + done + j;
            }
        }
        done += got;
        if (got < want) break;  // end of file
    }
    return std::nullopt;
}

bool FileReader::isAllZero(uint64_t offset, uint64_t length) const {
    std::vector<uint8_t> buf(kZeroBlock);
    uint64_t done = 0;
    while (done < length) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), length - done));
        const size_t got = read(offset + done, want, buf.data());
        if (got < want) return false;
        if (!allZero(buf.data(), got)) return false;
        done += got;
    }
    return true;
}

uint64_t FileReader::scan(std::string_view pattern, uint64_t begin, uint64_t length, uint64_t alignment,
                          const std::function<bool(uint64_t)>& onMatch) const {
    const size_t plen = pattern.size();
    if (plen == 0 || length == 0) return 0;
    if (alignment == 0) alignment = 1;
    const uint64_t end = (length > kToEnd - begin) ? kToEnd : begin + length;
    const auto* pat = reinterpret_cast<const uint8_t*>(pattern.data());

    // Anchor: the first non-zero pattern byte (state files are mostly zeros, so a zero anchor would hit constantly).
    size_t anchor = 0;
    while (anchor + 1 < plen && pat[anchor] == 0) anchor++;
    const int anchorByte = pat[anchor];

    std::vector<uint8_t> buf(kScanBlock + plen - 1);
    uint64_t base = begin;  // file offset of buf[0]
    size_t have = 0;
    uint64_t reported = 0;
    while (true) {
        const uint64_t remaining = end - (base + have);
        const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size() - have, remaining));
        const size_t got = want ? read(base + have, want, buf.data() + have) : 0;
        have += got;
        if (have >= plen) {
            const size_t lastStart = have - plen;
            size_t i = 0;
            while (i <= lastStart) {
                const void* hit = std::memchr(buf.data() + i + anchor, anchorByte, lastStart - i + 1);
                if (!hit) break;
                const size_t start = static_cast<size_t>(static_cast<const uint8_t*>(hit) - buf.data()) - anchor;
                if ((base + start) % alignment == 0 && std::memcmp(buf.data() + start, pat, plen) == 0) {
                    reported++;
                    if (!onMatch(base + start)) return reported;
                }
                i = start + 1;
            }
        }
        if (got < want || base + have >= end) break;  // end of file / end of range
        // Keep the last plen-1 bytes: a match starting in them needs bytes of the next block.
        const size_t keep = std::min(have, plen - 1);
        std::memmove(buf.data(), buf.data() + have - keep, keep);
        base += have - keep;
        have = keep;
    }
    return reported;
}

std::vector<uint64_t> FileReader::findAll(std::string_view pattern, uint64_t begin, uint64_t length, uint64_t alignment,
                                          size_t maxMatches) const {
    std::vector<uint64_t> result;
    if (maxMatches == 0) return result;
    scan(pattern, begin, length, alignment, [&](uint64_t offset) {
        result.push_back(offset);
        return result.size() < maxMatches;
    });
    return result;
}

}  // namespace qstate::support
