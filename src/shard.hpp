// shard.hpp
#pragma once

#include "eviction.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cache {

// ─────────────────────────────────────────────────────────────────────────────
// Shard — one thread-safe bucket of the sharded cache.
//
// Responsibilities (each kept to one concern):
//   • Stores string values keyed by string.
//   • Enforces a per-shard capacity + LRU eviction via LRUCache.
//   • Optionally attaches a TTL (expiry deadline) to any key.
//   • Protects all state with a readers-writer lock (shared_mutex):
//       GET / exists / ttl  →  shared lock   (many readers in parallel)
//       set / del / expire  →  unique lock   (one writer, excludes all)
//
// Thread-safety: fully thread-safe. Multiple threads may call any method
// concurrently without external synchronisation.
//
// TTL semantics — dual expiry strategy (mirrors real Redis):
//   • Lazy expiration:  key is evicted on the first access after its deadline.
//   • Active expiration: background thread calls purgeExpired() periodically.
//
// Why LRUCache::get() needs a unique lock (not shared):
//   LRUCache::get() splices the accessed node to the front of the list —
//   that is a mutation. shared_mutex does not allow concurrent mutations,
//   so all callers that touch LRUCache use a unique_lock, even reads.
//   The shared lock is reserved for pure metadata queries (exists, ttl)
//   that don't touch LRUCache internals.
// ─────────────────────────────────────────────────────────────────────────────

class Shard {
public:
    using Clock      = std::chrono::steady_clock;
    using TimePoint  = Clock::time_point;
    using Millis     = std::chrono::milliseconds;

    explicit Shard(std::size_t capacity)
        : lru_(capacity) {}

    // Non-copyable, non-movable: shared_mutex cannot be copied or moved
    // while potentially in use.
    Shard(const Shard&)            = delete;
    Shard& operator=(const Shard&) = delete;
    Shard(Shard&&)                 = delete;
    Shard& operator=(Shard&&)      = delete;

    ~Shard() = default;

    // ── Write operations (unique lock) ────────────────────────────────────────

    // Insert or overwrite key → value with no expiry.
    void set(const std::string& key, std::string value) {
        std::unique_lock lock(mutex_);
        ttl_.erase(key);                        // clear any previous TTL
        lru_.put(key, std::move(value));
    }

    // Insert or overwrite key → value, expiring after `ttl` milliseconds.
    void setWithTTL(const std::string& key, std::string value, Millis ttl) {
        std::unique_lock lock(mutex_);
        lru_.put(key, std::move(value));
        ttl_[key] = Clock::now() + ttl;
    }

    // Remove a key. No-op if absent.
    void del(const std::string& key) {
        std::unique_lock lock(mutex_);
        lru_.erase(key);
        ttl_.erase(key);
    }

    // Attach (or replace) a TTL on an existing, non-expired key.
    // Returns true if the key was found and updated, false otherwise.
    bool expire(const std::string& key, Millis ttl) {
        std::unique_lock lock(mutex_);
        if (!lru_.contains(key) || isExpiredUnlocked(key)) return false;
        ttl_[key] = Clock::now() + ttl;
        return true;
    }

    // Remove the TTL from a key, making it persistent.
    // Returns true if the key existed and had a TTL removed.
    bool persist(const std::string& key) {
        std::unique_lock lock(mutex_);
        if (!lru_.contains(key) || isExpiredUnlocked(key)) return false;
        return ttl_.erase(key) > 0;
    }

    // ── Read operations ───────────────────────────────────────────────────────

    // Returns the value if the key exists and has not expired.
    // Lazily removes the entry if it is expired.
    // Uses a unique lock because LRUCache::get() mutates list order.
    std::optional<std::string> get(const std::string& key) {
        std::unique_lock lock(mutex_);
        if (!lru_.contains(key)) return std::nullopt;
        if (isExpiredUnlocked(key)) {
            lru_.erase(key);
            ttl_.erase(key);
            return std::nullopt;
        }
        return lru_.get(key);   // also promotes key to MRU
    }

    // Returns true if the key exists and is not expired.
    // Uses a shared lock — contains() is a pure read on unordered_map.
    bool exists(const std::string& key) {
        std::shared_lock lock(mutex_);
        return lru_.contains(key) && !isExpiredUnlocked(key);
    }

    // Returns remaining TTL in milliseconds, or:
    //   -1  →  key exists but has no TTL (persistent)
    //   -2  →  key does not exist or is expired
    long long ttl(const std::string& key) {
        std::shared_lock lock(mutex_);
        if (!lru_.contains(key) || isExpiredUnlocked(key)) return -2LL;
        auto it = ttl_.find(key);
        if (it == ttl_.end()) return -1LL;
        auto remaining = std::chrono::duration_cast<Millis>(
            it->second - Clock::now());
        return remaining.count() > 0LL ? remaining.count() : 0LL;
    }

    // ── Snapshot (used by persistence layer) ─────────────────────────────────

    struct Entry {
        std::string key;
        std::string value;
        long long   ttlMs;   // -1 = persistent, >0 = remaining ms
    };

    // Return a point-in-time copy of all live, non-expired entries.
    // Expired keys are skipped. Called by persistence::save() under
    // a unique lock so the snapshot is consistent.
    std::vector<Entry> snapshot() {
        std::unique_lock lock(mutex_);
        std::vector<Entry> entries;
        entries.reserve(lru_.size());

        // Iterate TTL map first to collect expired keys to skip
        auto now = Clock::now();

        // Walk all entries via a temporary get loop — but LRUCache has no
        // iterator. Instead, use a side-channel: we know every key in lru_
        // is also in index_. We expose a forEachKey() from LRUCache.
        lru_.forEach([&](const std::string& key, const std::string& val) {
            // Skip expired
            auto it = ttl_.find(key);
            if (it != ttl_.end() && now >= it->second) return;

            long long ttlMs = -1LL;
            if (it != ttl_.end()) {
                auto remaining = std::chrono::duration_cast<Millis>(
                    it->second - now);
                ttlMs = remaining.count() > 0 ? remaining.count() : 0LL;
            }
            entries.push_back({key, val, ttlMs});
        });
        return entries;
    }

    // Sweep all keys with a TTL and remove those that have expired.
    // Returns the number of keys purged.
    std::size_t purgeExpired() {
        std::unique_lock lock(mutex_);
        std::vector<std::string> expired;
        expired.reserve(ttl_.size());
        for (auto& [key, deadline] : ttl_) {
            if (Clock::now() >= deadline) expired.push_back(key);
        }
        for (auto& key : expired) {
            lru_.erase(key);
            ttl_.erase(key);
        }
        return expired.size();
    }

    // ── Observers ────────────────────────────────────────────────────────────

    // Number of live (non-expired) keys currently held.
    // Note: may include not-yet-lazily-evicted expired entries; use
    // purgeExpired() first for an exact count.
    std::size_t size() {
        std::shared_lock lock(mutex_);
        return lru_.size();
    }

    std::size_t capacity() const noexcept {
        return lru_.capacity();
    }

private:
    // True if key has a TTL entry and that deadline has passed.
    // Caller must hold mutex_ in any mode.
    bool isExpiredUnlocked(const std::string& key) const {
        auto it = ttl_.find(key);
        if (it == ttl_.end()) return false;
        return Clock::now() >= it->second;
    }

    // ── State ─────────────────────────────────────────────────────────────────
    mutable std::shared_mutex                  mutex_;
    LRUCache<std::string, std::string>         lru_;
    std::unordered_map<std::string, TimePoint> ttl_;
};

}  // namespace cache
