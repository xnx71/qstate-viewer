// Bounded LRU cache for expensive derived data (flag scans, live-slot lists, sorted table indexes, ...).
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace qstate::decode {

// Thread-safe. Values are immutable shared objects; `bytes` is the caller's estimate of the memory they hold.
// An entry larger than the whole capacity is not stored. Keys are free text; by convention they start with
// "<scope>|<generation>|" so that invalidateScope() can drop everything of one decoder.
class DecodeCache {
public:
    static constexpr std::size_t kDefaultCapacity = 256u << 20;

    explicit DecodeCache(std::size_t capacityBytes = kDefaultCapacity) : capacity_(capacityBytes) {}

    std::shared_ptr<const void> get(const std::string& key);
    void put(const std::string& key, std::shared_ptr<const void> value, std::size_t bytes);

    template <class T>
    std::shared_ptr<const T> getAs(const std::string& key) {
        return std::static_pointer_cast<const T>(get(key));
    }

    // Removes every entry whose key starts with `prefix`.
    void erasePrefix(const std::string& prefix);
    void clear();
    void setCapacity(std::size_t capacityBytes);

    std::size_t usedBytes() const;
    std::size_t entryCount() const;
    std::size_t capacity() const;

private:
    struct Entry {
        std::string key;
        std::shared_ptr<const void> value;
        std::size_t bytes;
    };
    void evictLocked();

    mutable std::mutex mutex_;
    std::list<Entry> lru_; // front = most recently used
    std::unordered_map<std::string, std::list<Entry>::iterator> map_;
    std::size_t used_ = 0;
    std::size_t capacity_;
};

} // namespace qstate::decode
