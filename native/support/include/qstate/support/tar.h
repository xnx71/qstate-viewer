// Minimal streaming tar reader that extracts into a directory (enough for `git archive`: ustar headers, pax
// extended headers 'x' / 'g' and GNU long names 'L'). Symbolic links, hard links and special files are skipped
// (never created); entries whose path is absolute or contains ".." fail the extraction.
//
// Thread-safety: one extractor per thread (no internal locking).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace qstate::support {

class TarExtractor {
public:
    // `destDir` must exist (it is created when it does not).
    explicit TarExtractor(std::string destDir);
    ~TarExtractor();
    TarExtractor(const TarExtractor&) = delete;
    TarExtractor& operator=(const TarExtractor&) = delete;

    // Consumes the next piece of the tar stream. Returns false once an error occurred (see error()).
    bool feed(const char* data, size_t size);
    // Signals the end of the stream: false when an error occurred or the stream ended inside an entry.
    bool finish();

    const std::string& error() const { return error_; }
    size_t filesWritten() const { return files_; }
    size_t directoriesCreated() const { return dirs_; }
    size_t skippedEntries() const { return skipped_; }
    uint64_t bytesWritten() const { return bytes_; }

private:
    enum class State { Header, Data, Padding, End };
    enum class Kind { File, Directory, PaxLocal, PaxGlobal, LongName, Skip };

    void fail(const std::string& message);
    void processHeader();
    void beginEntry(Kind kind, uint64_t size);
    void consumeData(const char* data, size_t size);
    void endEntry();
    void parsePax(const std::string& records);
    bool resolvePath(const std::string& name, std::string& out);
    void closeFile();

    std::string dest_;
    std::string error_;
    State state_ = State::Header;
    Kind kind_ = Kind::Skip;
    char header_[512];
    size_t headerFill_ = 0;
    int zeroBlocks_ = 0;
    uint64_t remaining_ = 0;
    size_t padding_ = 0;
    std::string meta_;          // pax records / long name being collected
    std::string pendingPath_;   // path from the previous 'x' / 'L' header
    bool hasPendingPath_ = false;
    std::string entryPath_;     // absolute target of the current file
    std::FILE* file_ = nullptr;
    size_t files_ = 0, dirs_ = 0, skipped_ = 0;
    uint64_t bytes_ = 0;
    bool finished_ = false;
};

} // namespace qstate::support
