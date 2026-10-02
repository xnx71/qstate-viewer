// Test helpers for real state files.
#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>

#include "layout_loader.h"
#include "qstate/decode/byte_source.h"
#include "qstate/decode/decoder.h"
#include "qstate/decode/identity.h"
#include "qstate/decode/support_glue.h"
#include "test_env.h"

namespace qstate::decode::testing {

// pread based source (thread-safe), independent of qstate::support.
class PreadSource final : public ByteSource {
public:
    explicit PreadSource(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ >= 0) {
            struct stat st;
            if (::fstat(fd_, &st) == 0) size_ = static_cast<std::uint64_t>(st.st_size);
        }
    }
    ~PreadSource() override {
        if (fd_ >= 0) ::close(fd_);
    }
    bool ok() const { return fd_ >= 0; }
    std::uint64_t size() const override { return size_; }
    std::size_t read(std::uint64_t offset, std::size_t length, std::uint8_t* out) const override {
        std::size_t done = 0;
        while (done < length) {
            const ssize_t n = ::pread(fd_, out + done, length - done, static_cast<off_t>(offset + done));
            if (n <= 0) break;
            done += static_cast<std::size_t>(n);
        }
        return done;
    }

private:
    int fd_ = -1;
    std::uint64_t size_ = 0;
};

inline std::shared_ptr<const IdentityCodec> realCodec() {
#if defined(QSTATE_DECODE_HAS_SUPPORT) && QSTATE_DECODE_HAS_SUPPORT
    return std::make_shared<SupportIdentityCodec>();
#else
    return std::make_shared<BasicIdentityCodec>();
#endif
}
constexpr bool kRealIdentities =
#if defined(QSTATE_DECODE_HAS_SUPPORT) && QSTATE_DECODE_HAS_SUPPORT
    true;
#else
    false;
#endif

struct RealContract {
    LoadedContract lc;
    std::shared_ptr<PreadSource> src;
    std::unique_ptr<StateDecoder> dec;
    std::string file;
    bool ok = false;
};

// Loads contract `index` of epoch 229 with the ground-truth layouts. ok == false when data are not configured.
inline RealContract openReal(int index, std::shared_ptr<DecodeCache> cache = nullptr) {
    RealContract r;
    const std::string stateDir = qstate::testing::stateDir(), srcDir = qstate::testing::sourceDir();
    if (stateDir.empty() || srcDir.empty()) return r;
    char name[64];
    std::snprintf(name, sizeof name, "/contract%04d.229", index);
    r.file = stateDir + name;
    r.src = std::make_shared<PreadSource>(r.file);
    if (!r.src->ok()) return r;
    const bool inA = index <= 15;
    LayoutLoader loader(srcDir + "/docs/research/data/contract-layouts-" + (inA ? "a" : "b") + ".json", "v1.303.2");
    if (!loader.hasContract(index)) return r;
    r.lc = loader.load(index);
    DecoderConfig cfg;
    cfg.identity = realCodec();
    cfg.cache = cache;
    cfg.scope = static_cast<std::uint32_t>(index);
    cfg.contractName = [](std::uint32_t i) { return i == 1 ? std::string("QX") : std::string(); };
    r.dec = std::make_unique<StateDecoder>(r.lc.schema, r.lc.root, r.src, cfg);
    r.ok = true;
    return r;
}

} // namespace qstate::decode::testing
