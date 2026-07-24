// cache.hpp
#pragma once

#include "shard.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace cache {

// ─────────────────────────────────────────────────────────────────────────────
// ShardedCache — the top-level, fully concurrent in-memory cache.
//
// Architecture
// ────────────
//   Instead of one map behind one lock, keyspace is split across N independent
//   Shard instances. A key is routed to exactly one shard by:
//
//       shardIndex = std::hash<string>(key) % numShards
//
//   Why this matters for throughput:
//     With 1 shard: every SET from any client serialises behind one lock.
//     With N shards: writes to keys in different shards run truly in parallel.
//     Contention drops by ~N on a uniformly distributed workload.
//
// Shard count
// ───────────
//   Must be a power of two (enforced in constructor). This lets us replace
//   the modulo with a bitmask:
//
//       shardIndex = hash & (numShards - 1)   // cheaper than %
//
//   Default is 16 shards — a good balance for most workloads. Increase
//   for very high thread counts (64–256).
//
// Capacity
// ────────
//   totalCapacity is divided evenly across shards (floor division).
//   Each shard enforces its own LRU limit independently.
//   Minimum 1 entry per shard is guaranteed.
//
// Thread-safety
// ─────────────
//   Fully thread-safe. All synchronisation is inside Shard; ShardedCache
//   itself holds no locks — it simply routes and delegates.
// ─────────────────────────────────────────────────────────────────────────────

class ShardedCache {
public:
    using Millis = std::chrono::milliseconds;

    // numShards must be a power of two and >= 1.
    // totalCapacity is distributed evenly across shards.
    explicit ShardedCache(std::size_t totalCapacity,
                          std::size_t numShards = 16)
        : numShards_(numShards)
        , mask_(numShards - 1)
    {
        if (numShards_ == 0 || (numShards_ & mask_) != 0) {
            throw std::invalid_argument(
                "ShardedCache: numShards must be a power of two >= 1");
        }
        if (totalCapacity == 0) {
            throw std::invalid_argument(
                "ShardedCache: totalCapacity must be > 0");
        }

        // Per-shard capacity: at least 1 slot even if totalCapacity < numShards.
        std::size_t perShard = std::max(std::size_t{1},
                                        totalCapacity / numShards_);

        shards_.reserve(numShards_);
        for (std::size_t i = 0; i < numShards_; ++i) {
            shards_.emplace_back(std::make_unique<Shard>(perShard));
        }
    }

    // Non-copyable, non-movable (Shard is non-movable).
    ShardedCache(const ShardedCache&)            = delete;
    ShardedCache& operator=(const ShardedCache&) = delete;
    ShardedCache(ShardedCache&&)                 = delete;
    ShardedCache& operator=(ShardedCache&&)      = delete;

    ~ShardedCache() = default;

    // ── Write operations ──────────────────────────────────────────────────────

    void set(const std::string& key, std::string value) {
        shardFor(key).set(key, std::move(value));
    }

    void setWithTTL(const std::string& key, std::string value, Millis ttl) {
        shardFor(key).setWithTTL(key, std::move(value), ttl);
    }

    void del(const std::string& key) {
        shardFor(key).del(key);
    }

    // Returns true if the key existed and its TTL was updated.
    bool expire(const std::string& key, Millis ttl) {
        return shardFor(key).expire(key, ttl);
    }

    // Returns true if the key existed and had its TTL removed.
    bool persist(const std::string& key) {
        return shardFor(key).persist(key);
    }

    // ── Read operations ───────────────────────────────────────────────────────

    std::optional<std::string> get(const std::string& key) {
        return shardFor(key).get(key);
    }

    bool exists(const std::string& key) {
        return shardFor(key).exists(key);
    }

    // Returns remaining TTL in milliseconds, -1 (persistent), or -2 (missing).
    long long ttl(const std::string& key) {
        return shardFor(key).ttl(key);
    }

    // ── Snapshot (used by persistence) ───────────────────────────────────────

    using SnapshotEntry = Shard::Entry;

    // Collect all live entries across every shard.
    // Not atomic across shards but consistent per-shard (each shard locks itself).
    std::vector<SnapshotEntry> snapshot() {
        std::vector<SnapshotEntry> all;
        for (auto& shard : shards_) {
            auto entries = shard->snapshot();
            all.insert(all.end(),
                       std::make_move_iterator(entries.begin()),
                       std::make_move_iterator(entries.end()));
        }
        return all;
    }

    // Replay a snapshot back into the cache (used on startup after load).
    void restore(const std::vector<SnapshotEntry>& entries) {
        for (auto& e : entries) {
            if (e.ttlMs > 0) {
                setWithTTL(e.key, e.value, Millis{e.ttlMs});
            } else {
                set(e.key, e.value);
            }
        }
    }

    // ── Maintenance ───────────────────────────────────────────────────────────

    // Sweep all shards and remove expired keys.
    // Called periodically by the background expiry thread (expiry.hpp).
    // Returns total number of keys purged across all shards.
    std::size_t purgeExpired() {
        std::size_t total = 0;
        for (auto& shard : shards_) {
            total += shard->purgeExpired();
        }
        return total;
    }

    // ── Observers ────────────────────────────────────────────────────────────

    // Total live keys across all shards (approximate — no global lock).
    std::size_t size() const {
        std::size_t total = 0;
        for (auto& shard : shards_) {
            total += shard->size();
        }
        return total;
    }

    std::size_t numShards()     const noexcept { return numShards_; }
    std::size_t shardCapacity() const noexcept { return shards_[0]->capacity(); }

    // Index of the shard that owns this key. Useful for tests and diagnostics.
    std::size_t shardIndex(const std::string& key) const {
        return hasher_(key) & mask_;
    }

private:
    Shard& shardFor(const std::string& key) {
        return *shards_[hasher_(key) & mask_];
    }

    const Shard& shardFor(const std::string& key) const {
        return *shards_[hasher_(key) & mask_];
    }

    // ── State ─────────────────────────────────────────────────────────────────
    const std::size_t                    numShards_;
    const std::size_t                    mask_;       // numShards_ - 1
    std::vector<std::unique_ptr<Shard>>  shards_;
    std::hash<std::string>               hasher_;
};

}  // namespace cache
