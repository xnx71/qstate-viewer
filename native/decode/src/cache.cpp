#include "qstate/decode/cache.h"

namespace qstate::decode {

std::shared_ptr<const void> DecodeCache::get(const std::string& key) {
    std::lock_guard lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->value;
}

void DecodeCache::put(const std::string& key, std::shared_ptr<const void> value, std::size_t bytes) {
    std::lock_guard lock(mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) {
        used_ -= it->second->bytes;
        lru_.erase(it->second);
        map_.erase(it);
    }
    if (bytes > capacity_) return;
    lru_.push_front(Entry{key, std::move(value), bytes});
    map_[key] = lru_.begin();
    used_ += bytes;
    evictLocked();
}

void DecodeCache::evictLocked() {
    while (used_ > capacity_ && !lru_.empty()) {
        Entry& e = lru_.back();
        used_ -= e.bytes;
        map_.erase(e.key);
        lru_.pop_back();
    }
}

void DecodeCache::erasePrefix(const std::string& prefix) {
    std::lock_guard lock(mutex_);
    for (auto it = lru_.begin(); it != lru_.end();) {
        if (it->key.compare(0, prefix.size(), prefix) == 0) {
            used_ -= it->bytes;
            map_.erase(it->key);
            it = lru_.erase(it);
        } else {
            ++it;
        }
    }
}

void DecodeCache::clear() {
    std::lock_guard lock(mutex_);
    lru_.clear();
    map_.clear();
    used_ = 0;
}

void DecodeCache::setCapacity(std::size_t capacityBytes) {
    std::lock_guard lock(mutex_);
    capacity_ = capacityBytes;
    evictLocked();
}

std::size_t DecodeCache::usedBytes() const {
    std::lock_guard lock(mutex_);
    return used_;
}
std::size_t DecodeCache::entryCount() const {
    std::lock_guard lock(mutex_);
    return map_.size();
}
std::size_t DecodeCache::capacity() const {
    std::lock_guard lock(mutex_);
    return capacity_;
}

} // namespace qstate::decode
