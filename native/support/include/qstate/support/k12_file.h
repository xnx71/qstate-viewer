// KangarooTwelve digest of a whole file (the contract state digest used by the node), hashing the 8192-byte leaves
// on several threads. Memory use is bounded (a few MiB per thread plus 32 bytes per 8 KiB of file).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "qstate/support/file_reader.h"
#include "qstate/support/k12.h"

namespace qstate::support {

struct K12FileOptions {
    // Worker threads; 0 = min(hardware concurrency, 8).
    unsigned threads = 0;
    // Called with (bytesDone, bytesTotal), serialised, from worker threads. Return false to cancel.
    std::function<bool(uint64_t, uint64_t)> progress;
};

// Hashes reader.size() bytes (the size as of the last open / refresh). Returns nullopt when cancelled by the
// progress callback. Throws FileError on I/O errors or when the file turns out to be shorter than size().
std::optional<K12Digest> k12DigestFile(const FileReader& reader, const K12FileOptions& options = {});

} // namespace qstate::support
