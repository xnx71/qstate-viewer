// RAII temporary directory for tests (created under the system temp directory, removed on destruction).
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace testutil {

class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("qstate-support-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ec;
        // Tests may leave read-only directories behind.
        for (auto it = std::filesystem::recursive_directory_iterator(path_, ec); !ec && it != std::filesystem::end(it);
             it.increment(ec)) {
            std::error_code pe;
            std::filesystem::permissions(it->path(), std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::add, pe);
        }
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace testutil
