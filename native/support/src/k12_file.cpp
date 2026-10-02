#include "qstate/support/k12_file.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace qstate::support {

namespace {

constexpr size_t kChunksPerBlock = 256;  // 2 MiB of file per work unit

[[noreturn]] void throwTruncated(const FileReader& reader) {
    throw FileError("'" + reader.path() + "' is shorter than expected (truncated while hashing)", 0);
}

}  // namespace

std::optional<K12Digest> k12DigestFile(const FileReader& reader, const K12FileOptions& options) {
    const uint64_t length = reader.size();
    if (length < kK12ChunkSize) {
        std::vector<uint8_t> data(static_cast<size_t>(length));
        if (reader.read(0, data.size(), data.data()) != data.size()) throwTruncated(reader);
        if (options.progress && !options.progress(length, length)) return std::nullopt;
        return k12Digest(data.data(), data.size());
    }

    std::vector<uint8_t> first(kK12ChunkSize);
    if (reader.read(0, first.size(), first.data()) != first.size()) throwTruncated(reader);

    const uint64_t n = k12ChunkCount(length);
    std::vector<uint8_t> cvs(static_cast<size_t>(n) * kK12CvSize);

    // Chunks 1..fullChunks are completely inside the message; the last chunk (index n) carries the length encoding.
    const uint64_t fullChunks = (length - kK12ChunkSize) / kK12ChunkSize;
    const uint64_t blocks = (fullChunks + kChunksPerBlock - 1) / kChunksPerBlock;

    std::atomic<uint64_t> nextBlock{0};
    std::atomic<uint64_t> bytesDone{kK12ChunkSize};
    std::atomic<bool> stop{false};
    std::atomic<bool> cancelled{false};
    std::exception_ptr failure;
    std::mutex mutex;  // guards `failure` and serialises progress callbacks

    auto report = [&](uint64_t delta) {
        const uint64_t done = bytesDone.fetch_add(delta) + delta;
        if (!options.progress) return;
        std::lock_guard<std::mutex> lock(mutex);
        if (stop.load()) return;
        if (!options.progress(std::min(done, length), length)) {
            cancelled = true;
            stop = true;
        }
    };

    auto worker = [&] {
        try {
            std::vector<uint8_t> buf(kChunksPerBlock * kK12ChunkSize);
            while (!stop.load()) {
                const uint64_t block = nextBlock.fetch_add(1);
                if (block >= blocks) break;
                const uint64_t firstChunk = 1 + block * kChunksPerBlock;
                const size_t count = static_cast<size_t>(std::min<uint64_t>(kChunksPerBlock, fullChunks + 1 - firstChunk));
                const size_t bytes = count * kK12ChunkSize;
                if (reader.read(firstChunk * kK12ChunkSize, bytes, buf.data()) != bytes) throwTruncated(reader);
                k12LeafChainingValues(buf.data(), count, cvs.data() + (firstChunk - 1) * kK12CvSize);
                report(bytes);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!failure) failure = std::current_exception();
            stop = true;
        }
    };

    unsigned threads = options.threads ? options.threads : std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
    threads = static_cast<unsigned>(std::min<uint64_t>(threads, std::max<uint64_t>(blocks, 1)));
    if (threads <= 1) {
        worker();
    } else {
        std::vector<std::thread> pool;
        pool.reserve(threads - 1);
        for (unsigned i = 1; i < threads; i++) pool.emplace_back(worker);
        worker();
        for (auto& t : pool) t.join();
    }
    if (failure) std::rethrow_exception(failure);
    if (cancelled) return std::nullopt;

    // Last chunk: the remaining message bytes (0..8191) followed by the 0x00 of length_encode(0).
    {
        const uint64_t consumed = kK12ChunkSize + fullChunks * kK12ChunkSize;
        const size_t rest = static_cast<size_t>(length - consumed);
        std::vector<uint8_t> tail(rest);
        if (rest != 0 && reader.read(consumed, rest, tail.data()) != rest) throwTruncated(reader);
        k12LeafChainingValue(tail.data(), rest, true, cvs.data() + (n - 1) * kK12CvSize);
    }
    K12Digest digest;
    k12FromChainingValues(first.data(), cvs.data(), n, digest.data(), digest.size());
    if (options.progress) options.progress(length, length);
    return digest;
}

}  // namespace qstate::support
