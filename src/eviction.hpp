// eviction.hpp
#pragma once

#include <cstddef>
#include <list>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace cache {

// A generic, single-threaded LRU eviction policy.
//
// Thread-safety: this class is NOT thread-safe by design. Synchronization
// is the responsibility of the caller (e.g. Cache/Shard), keeping this
// class focused on one job: LRU bookkeeping. Mixing locking into this
// class would violate single responsibility and make it harder to test
// or reuse (e.g. inside an already-locked shard).

template <typename Key, typename Value>
class LRUCache {
public:
    explicit LRUCache(std::size_t capacity)
        : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("LRUCache capacity must be > 0");
        }
    }

    // Non-copyable: copying would require deep-copying the list and
    // rebuilding the map's iterators, which is error-prone and rarely
    // what callers actually want. Move is cheap and sufficient.
    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;
    LRUCache(LRUCache&&) noexcept = default;
    LRUCache& operator=(LRUCache&&) noexcept = default;
    ~LRUCache() = default;

    // Returns the value if present, and marks it as most recently used.
    std::optional<Value> get(const Key& key) {
        auto it = index_.find(key);
        if (it == index_.end()) {
            return std::nullopt;
        }
        touch(it->second);
        return it->second->second;
    }

    // Inserts or updates a key. Evicts the least-recently-used entry
    // if the cache is at capacity and this is a new key.
    void put(const Key& key, Value value) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->second = std::move(value);
            touch(it->second);
            return;
        }

        if (items_.size() >= capacity_) {
            evictLRU();
        }

        items_.emplace_front(key, std::move(value));
        index_[key] = items_.begin();
    }

    // Removes a key from the cache. No-op if the key is not present.
    void erase(const Key& key) {
        auto it = index_.find(key);
        if (it == index_.end()) return;
        items_.erase(it->second);
        index_.erase(it);
    }

    bool contains(const Key& key) const {
        return index_.find(key) != index_.end();
    }

    std::size_t size() const noexcept {
        return items_.size();
    }

    std::size_t capacity() const noexcept {
        return capacity_;
    }

private:
    using ListType = std::list<std::pair<Key, Value>>;
    using ListIterator = typename ListType::iterator;

    // Move an existing entry to the front (most-recently-used position).
    void touch(ListIterator it) {
        items_.splice(items_.begin(), items_, it);
    }
    
    void evictLRU() {
        auto lru = std::prev(items_.end());
        index_.erase(lru->first);
        items_.pop_back();
    }

    std::size_t capacity_;
    ListType items_;                                     // front = MRU, back = LRU
    std::unordered_map<Key, ListIterator> index_;        // key -> position in items_
};

}  // namespace cache