// tests/test_cache.cpp
//
// Unit + concurrency tests for cache::ShardedCache (src/cache.hpp).
//
// Test groups:
//   1.  Construction — valid/invalid arguments
//   2.  set / get basics
//   3.  del
//   4.  exists
//   5.  LRU eviction through the ShardedCache API
//   6.  TTL — setWithTTL
//   7.  TTL — expire() / persist()
//   8.  TTL — ttl() query
//   9.  purgeExpired()
//  10.  Sharding — key distribution and routing consistency
//  11.  Concurrent writes
//  12.  Concurrent reads + writes
//  13.  Concurrent purgeExpired under load

#include "cache.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

using cache::ShardedCache;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// 1. Construction
// ─────────────────────────────────────────────────────────────────────────────

TEST(CacheConstruction, DefaultNumShards) {
    ShardedCache c(1024);
    EXPECT_EQ(c.numShards(), 16u);
}

TEST(CacheConstruction, CustomNumShards) {
    ShardedCache c(256, 4);
    EXPECT_EQ(c.numShards(), 4u);
}

TEST(CacheConstruction, PerShardCapacityIsDistributed) {
    // 256 total capacity, 8 shards → 32 per shard
    ShardedCache c(256, 8);
    EXPECT_EQ(c.shardCapacity(), 32u);
}

TEST(CacheConstruction, ZeroCapacityThrows) {
    EXPECT_THROW({ ShardedCache c(0, 4); (void)c; }, std::invalid_argument);
}

TEST(CacheConstruction, NonPowerOfTwoShardsThrows) {
    EXPECT_THROW({ ShardedCache c(64, 3); (void)c; }, std::invalid_argument);
}

TEST(CacheConstruction, ZeroShardsThrows) {
    EXPECT_THROW({ ShardedCache c(64, 0); (void)c; }, std::invalid_argument);
}

TEST(CacheConstruction, SingleShardIsValid) {
    EXPECT_NO_THROW({ ShardedCache c(8, 1); (void)c; });
}

TEST(CacheConstruction, StartsEmpty) {
    ShardedCache c(128, 4);
    EXPECT_EQ(c.size(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. set / get basics
// ─────────────────────────────────────────────────────────────────────────────

TEST(CacheSetGet, GetMissingReturnsNullopt) {
    ShardedCache c(64);
    EXPECT_EQ(c.get("missing"), std::nullopt);
}

TEST(CacheSetGet, GetAfterSet) {
    ShardedCache c(64);
    c.set("hello", "world");
    ASSERT_NE(c.get("hello"), std::nullopt);
    EXPECT_EQ(*c.get("hello"), "world");
}

TEST(CacheSetGet, OverwriteUpdatesValue) {
    ShardedCache c(64);
    c.set("k", "v1");
    c.set("k", "v2");
    EXPECT_EQ(*c.get("k"), "v2");
}

TEST(CacheSetGet, MultipleDistinctKeys) {
    ShardedCache c(64);
    for (int i = 0; i < 20; ++i) {
        c.set(std::to_string(i), "val_" + std::to_string(i));
    }
    for (int i = 0; i < 20; ++i) {
        ASSERT_NE(c.get(std::to_string(i)), std::nullopt) << "missing key " << i;
        EXPECT_EQ(*c.get(std::to_string(i)), "val_" + std::to_string(i));
    }
}

TEST(CacheSetGet, SizeTracksInsertions) {
    ShardedCache c(64, 4);
    EXPECT_EQ(c.size(), 0u);
    c.set("a", "1");
    c.set("b", "2");
    c.set("c", "3");
    EXPECT_EQ(c.size(), 3u);
}

TEST(CacheSetGet, SetClearsPreviousTTL) {
    ShardedCache c(64);
    c.setWithTTL("k", "v1", 50ms);
    c.set("k", "v2");
    EXPECT_EQ(c.ttl("k"), -1LL);   // persistent now
    EXPECT_EQ(*c.get("k"), "v2");
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. del
// ─────────────────────────────────────────────────────────────────────────────

TEST(CacheDel, DelExistingKey) {
    ShardedCache c(64);
    c.set("x", "y");
    c.del("x");
    EXPECT_EQ(c.get("x"), std::nullopt);
    EXPECT_FALSE(c.exists("x"));
}

TEST(CacheDel, DelMissingKeyIsNoOp) {
    ShardedCache c(64);
    EXPECT_NO_THROW(c.del("ghost"));
}

TEST(CacheDel, DelDecreasesSize) {
    ShardedCache c(64, 4);
    c.set("a", "1");
    c.set("b", "2");
    c.del("a");
    EXPECT_EQ(c.size(), 1u);
}

TEST(CacheDel, DelAlsoRemovesTTL) {
    ShardedCache c(64);
    c.setWithTTL("k", "v", 5000ms);
    c.del("k");
    EXPECT_EQ(c.ttl("k"), -2LL);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. exists
// ─────────────────────────────────────────────────────────────────────────────

TEST(CacheExists, MissingKeyReturnsFalse) {
    ShardedCache c(64);
    EXPECT_FALSE(c.exists("no"));
}

TEST(CacheExists, ExistingKeyReturnsTrue) {
    ShardedCache c(64);
    c.set("yes", "v");
    EXPECT_TRUE(c.exists("yes"));
}

TEST(CacheExists, AfterDelReturnsFalse) {
    ShardedCache c(64);
    c.set("k", "v");
    c.del("k");
    EXPECT_FALSE(c.exists("k"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. LRU eviction through ShardedCache API
// ─────────────────────────────────────────────────────────────────────────────

TEST(CacheLRU, EvictsWhenShardFull) {
    // Use 1 shard so eviction is deterministic (all keys compete in same shard).
    ShardedCache c(3, 1);
    c.set("a", "1");
    c.set("b", "2");
    c.set("c", "3");
    c.set("d", "4");    // evicts "a" (LRU)

    EXPECT_FALSE(c.exists("a"));
    EXPECT_TRUE(c.exists("b"));
    EXPECT_TRUE(c.exists("c"));
    EXPECT_TRUE(c.exists("d"));
}

TEST(CacheLRU, SizeNeverExceedsCapacity) {
    // 1 shard, capacity 5 — insert 50 keys, size should stay <= 5
    ShardedCache c(5, 1);
    for (int i = 0; i < 50; ++i) {
        c.set(std::to_string(i), "v");
    }
    EXPECT_LE(c.size(), 5u);
}

TEST(CacheLRU, GetPromotesAcrossShardedCache) {
    ShardedCache c(3, 1);
    c.set("a", "1");
    c.set("b", "2");
    c.set("c", "3");
    c.get("a");         // promote a → MRU; b becomes LRU
    c.set("d", "4");    // evicts b

    EXPECT_TRUE(c.exists("a"));
    EXPECT_FALSE(c.exists("b"));
    EXPECT_TRUE(c.exists("c"));
    EXPECT_TRUE(c.exists("d"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. TTL — setWithTTL
// ─────────────────────────────────────────────────────────────────────────────

TEST(CacheTTL, KeyAvailableBeforeExpiry) {
    ShardedCache c(64);
    c.setWithTTL("k", "v", 500ms);
    EXPECT_NE(c.get("k"), std::nullopt);
}

TEST(CacheTTL, KeyExpiredAfterTTL) {
    ShardedCache c(64);
    c.setWithTTL("k", "v", 50ms);
    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(c.get("k"), std::nullopt);
}

TEST(CacheTTL, MultipleKeysIndependentExpiry) {
    ShardedCache c(64);
    c.setWithTTL("short",   "s", 50ms);
    c.setWithTTL("medium",  "m", 400ms);
    c.set("forever",        "f");

    std::this_thread::sleep_for(80ms);

    EXPECT_EQ(c.get("short"),   std::nullopt);
    EXPECT_NE(c.get("medium"),  std::nullopt);
    EXPECT_NE(c.get("forever"), std::nullopt);
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. expire() / persist()
// ─────────────────────────────────────────────────────────────────────────────

TEST(CacheExpire, ExpireOnExistingKey) {
    ShardedCache c(64);
    c.set("k", "v");
    EXPECT_TRUE(c.expire("k", 50ms));
    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(c.get("k"), std::nullopt);
}

TEST(CacheExpire, ExpireOnMissingKeyReturnsFalse) {
    ShardedCache c(64);
    EXPECT_FALSE(c.expire("ghost", 100ms));
}

TEST(CachePersist, PersistRemovesTTL) {
    ShardedCache c(64);
    c.setWithTTL("k", "v", 500ms);
    EXPECT_TRUE(c.persist("k"));
    EXPECT_EQ(c.ttl("k"), -1LL);
}

TEST(CachePersist, PersistOnPersistentKeyReturnsFalse) {
    ShardedCache c(64);
    c.set("k", "v");
    EXPECT_FALSE(c.persist("k"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. ttl() query
// ─────────────────────────────────────────────────────────────────────────────

TEST(CacheTTLQuery, MissingKeyReturnsMinusTwo) {
    ShardedCache c(64);
    EXPECT_EQ(c.ttl("ghost"), -2LL);
}

TEST(CacheTTLQuery, PersistentKeyReturnsMinusOne) {
    ShardedCache c(64);
    c.set("k", "v");
    EXPECT_EQ(c.ttl("k"), -1LL);
}

TEST(CacheTTLQuery, ActiveTTLReturnsPositive) {
    ShardedCache c(64);
    c.setWithTTL("k", "v", 500ms);
    long long remaining = c.ttl("k");
    EXPECT_GT(remaining, 0LL);
    EXPECT_LE(remaining, 500LL);
}

TEST(CacheTTLQuery, ExpiredKeyReturnsMinusTwo) {
    ShardedCache c(64);
    c.setWithTTL("k", "v", 50ms);
    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(c.ttl("k"), -2LL);
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. purgeExpired()
// ─────────────────────────────────────────────────────────────────────────────

TEST(CachePurge, PurgesExpiredKeysAcrossAllShards) {
    ShardedCache c(256, 8);
    // Insert keys that will land in different shards
    for (int i = 0; i < 20; ++i) {
        c.setWithTTL("exp_" + std::to_string(i), "v", 50ms);
    }
    for (int i = 0; i < 5; ++i) {
        c.set("keep_" + std::to_string(i), "v");
    }

    std::this_thread::sleep_for(80ms);
    std::size_t purged = c.purgeExpired();

    EXPECT_EQ(purged, 20u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(c.exists("keep_" + std::to_string(i)));
    }
}

TEST(CachePurge, PurgeWithNoExpiredReturnsZero) {
    ShardedCache c(64);
    c.set("a", "1");
    c.set("b", "2");
    EXPECT_EQ(c.purgeExpired(), 0u);
}

TEST(CachePurge, SizeDecreasesAfterPurge) {
    ShardedCache c(256, 4);
    for (int i = 0; i < 10; ++i) {
        c.setWithTTL("e_" + std::to_string(i), "v", 50ms);
    }
    c.set("keep", "v");
    EXPECT_EQ(c.size(), 11u);

    std::this_thread::sleep_for(80ms);
    c.purgeExpired();
    EXPECT_EQ(c.size(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. Sharding — key distribution and routing consistency
// ─────────────────────────────────────────────────────────────────────────────

TEST(CacheSharding, SameKeyAlwaysRoutsToSameShard) {
    ShardedCache c(256, 8);
    std::size_t idx1 = c.shardIndex("my_key");
    std::size_t idx2 = c.shardIndex("my_key");
    EXPECT_EQ(idx1, idx2);
}

TEST(CacheSharding, ShardIndexInBounds) {
    ShardedCache c(256, 8);
    for (int i = 0; i < 100; ++i) {
        EXPECT_LT(c.shardIndex(std::to_string(i)), 8u);
    }
}

TEST(CacheSharding, KeysDistributeAcrossShards) {
    // Insert many keys and verify at least half the shards receive some keys.
    // With a good hash function and 100 keys across 8 shards, all shards
    // should get some — but we only assert > 4 to avoid flakiness.
    constexpr int N = 100;
    constexpr int SHARDS = 8;
    ShardedCache c(N * 2, SHARDS);

    std::vector<int> perShard(SHARDS, 0);
    for (int i = 0; i < N; ++i) {
        std::string key = "key_" + std::to_string(i);
        c.set(key, "v");
        ++perShard[c.shardIndex(key)];
    }

    int nonEmpty = 0;
    for (int count : perShard) {
        if (count > 0) ++nonEmpty;
    }
    EXPECT_GT(nonEmpty, SHARDS / 2)
        << "Expected keys to spread across more than half the shards";
}

TEST(CacheSharding, IndependentShardsDoNotInterfere) {
    // Two keys in different shards should be fully independent.
    ShardedCache c(64, 8);
    std::string k1 = "alpha", k2 = "beta";

    // Ensure they're in different shards — find two that differ.
    // If they happen to collide, pick different names.
    std::string ka = "aaaaa", kb = "bbbbb";
    // Just assert that operations on one don't affect the other.
    c.set(ka, "va");
    c.set(kb, "vb");
    c.del(ka);

    EXPECT_FALSE(c.exists(ka));
    EXPECT_TRUE(c.exists(kb));
    EXPECT_EQ(*c.get(kb), "vb");
}

TEST(CacheSharding, SingleShardBehavesLikeOneShard) {
    // With numShards=1, all keys land in the same shard.
    ShardedCache c(8, 1);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(c.shardIndex(std::to_string(i)), 0u);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 11. Concurrent writes
// ─────────────────────────────────────────────────────────────────────────────

TEST(CacheConcurrent, ConcurrentSetsNoRace) {
    constexpr int THREADS = 8;
    constexpr int OPS     = 500;
    ShardedCache c(THREADS * OPS, 16);

    std::vector<std::thread> workers;
    workers.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t) {
        workers.emplace_back([&c, t]() {
            for (int i = 0; i < OPS; ++i) {
                c.set("t" + std::to_string(t) + "_k" + std::to_string(i), "v");
            }
        });
    }
    for (auto& w : workers) w.join();

    EXPECT_LE(c.size(), static_cast<std::size_t>(THREADS * OPS));
}

TEST(CacheConcurrent, ConcurrentSetsSmallCapNoRace) {
    // Force heavy LRU eviction under concurrent writes.
    ShardedCache c(32, 8);   // only 4 slots per shard
    constexpr int THREADS = 8;
    constexpr int OPS     = 1000;

    std::vector<std::thread> workers;
    workers.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t) {
        workers.emplace_back([&c, t]() {
            for (int i = 0; i < OPS; ++i) {
                c.set("t" + std::to_string(t) + "_" + std::to_string(i), "x");
            }
        });
    }
    for (auto& w : workers) w.join();

    // Size must never exceed total capacity.
    EXPECT_LE(c.size(), 32u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 12. Concurrent reads + writes
// ─────────────────────────────────────────────────────────────────────────────

TEST(CacheConcurrent, ConcurrentReadsAndWritesNoRace) {
    constexpr int WRITER_THREADS = 4;
    constexpr int READER_THREADS = 8;
    constexpr int OPS            = 500;

    ShardedCache c(256, 16);
    for (int i = 0; i < 20; ++i) c.set(std::to_string(i), "init");

    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;

    // Writers
    for (int t = 0; t < WRITER_THREADS; ++t) {
        workers.emplace_back([&c, &stop, t]() {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                std::string key = "w" + std::to_string(t) + "_" + std::to_string(i % OPS);
                c.set(key, "v");
                if (i % 7 == 0) c.del(key);
                ++i;
            }
        });
    }

    // Readers
    for (int t = 0; t < READER_THREADS; ++t) {
        workers.emplace_back([&c, &stop, t]() {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                (void)c.get(std::to_string(i % 20));
                (void)c.exists("w" + std::to_string(t % WRITER_THREADS)
                               + "_" + std::to_string(i % OPS));
                (void)c.ttl(std::to_string(i % 20));
                ++i;
            }
        });
    }

    std::this_thread::sleep_for(200ms);
    stop.store(true, std::memory_order_relaxed);
    for (auto& w : workers) w.join();

    EXPECT_LE(c.size(), 256u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 13. Concurrent purgeExpired under load
// ─────────────────────────────────────────────────────────────────────────────

TEST(CacheConcurrent, ConcurrentTTLAndPurgeNoRace) {
    ShardedCache c(1024, 16);

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> totalPurged{0};

    // Simulated background expiry thread
    std::thread expiry([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            totalPurged.fetch_add(c.purgeExpired(), std::memory_order_relaxed);
            std::this_thread::sleep_for(10ms);
        }
    });

    // Writer threads: set keys with short TTLs
    constexpr int WRITERS = 8;
    std::vector<std::thread> writers;
    for (int t = 0; t < WRITERS; ++t) {
        writers.emplace_back([&c, t]() {
            for (int i = 0; i < 200; ++i) {
                c.setWithTTL("t" + std::to_string(t) + "_" + std::to_string(i),
                             "v", 30ms);
            }
        });
    }
    for (auto& w : writers) w.join();

    std::this_thread::sleep_for(100ms);
    stop.store(true, std::memory_order_relaxed);
    expiry.join();

    c.purgeExpired();   // final sweep
    EXPECT_EQ(c.size(), 0u) << "all short-TTL keys should have expired";
}
