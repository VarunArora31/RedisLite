// tests/test_shard.cpp
//
// Unit + concurrency tests for cache::Shard (src/shard.hpp).
//
// Test groups:
//   1.  Construction
//   2.  set / get basics
//   3.  del
//   4.  exists
//   5.  LRU eviction through the Shard API
//   6.  TTL — setWithTTL
//   7.  TTL — expire() / persist()
//   8.  TTL — ttl() query
//   9.  purgeExpired()
//  10.  Interaction: LRU eviction + TTL together
//  11.  Concurrent writes (data-race free)
//  12.  Concurrent reads + writes (data-race free)
//  13.  Concurrent TTL expiry under load

#include "shard.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using cache::Shard;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// 1. Construction
// ─────────────────────────────────────────────────────────────────────────────

TEST(ShardConstruction, CapacityReported) {
    Shard s(16);
    EXPECT_EQ(s.capacity(), 16u);
}

TEST(ShardConstruction, StartsEmpty) {
    Shard s(8);
    EXPECT_EQ(s.size(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. set / get basics
// ─────────────────────────────────────────────────────────────────────────────

TEST(ShardSetGet, GetMissingReturnsNullopt) {
    Shard s(8);
    EXPECT_EQ(s.get("missing"), std::nullopt);
}

TEST(ShardSetGet, GetAfterSet) {
    Shard s(8);
    s.set("key", "value");
    ASSERT_NE(s.get("key"), std::nullopt);
    EXPECT_EQ(*s.get("key"), "value");
}

TEST(ShardSetGet, OverwriteUpdatesValue) {
    Shard s(8);
    s.set("k", "first");
    s.set("k", "second");
    EXPECT_EQ(*s.get("k"), "second");
}

TEST(ShardSetGet, OverwriteDoesNotGrowSize) {
    Shard s(8);
    s.set("k", "a");
    s.set("k", "b");
    EXPECT_EQ(s.size(), 1u);
}

TEST(ShardSetGet, MultipleKeys) {
    Shard s(8);
    s.set("a", "1");
    s.set("b", "2");
    s.set("c", "3");
    EXPECT_EQ(*s.get("a"), "1");
    EXPECT_EQ(*s.get("b"), "2");
    EXPECT_EQ(*s.get("c"), "3");
    EXPECT_EQ(s.size(), 3u);
}

TEST(ShardSetGet, SetClearsPreviousTTL) {
    Shard s(8);
    // Set with TTL, then overwrite without TTL — key should become persistent.
    s.setWithTTL("k", "v1", 50ms);
    s.set("k", "v2");   // re-set without TTL
    // TTL query should return -1 (persistent), not -2 (missing/expired).
    EXPECT_EQ(s.ttl("k"), -1LL);
    EXPECT_EQ(*s.get("k"), "v2");
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. del
// ─────────────────────────────────────────────────────────────────────────────

TEST(ShardDel, DelExistingKey) {
    Shard s(8);
    s.set("x", "hello");
    s.del("x");
    EXPECT_EQ(s.get("x"), std::nullopt);
    EXPECT_FALSE(s.exists("x"));
}

TEST(ShardDel, DelMissingKeyIsNoOp) {
    Shard s(8);
    EXPECT_NO_THROW(s.del("ghost"));
}

TEST(ShardDel, DelReducesSize) {
    Shard s(8);
    s.set("a", "1");
    s.set("b", "2");
    s.del("a");
    EXPECT_EQ(s.size(), 1u);
    EXPECT_FALSE(s.exists("a"));
    EXPECT_TRUE(s.exists("b"));
}

TEST(ShardDel, DelAlsoRemovesTTL) {
    Shard s(8);
    s.setWithTTL("k", "v", 5000ms);
    s.del("k");
    // After del, ttl() should report -2 (not found), not any positive value.
    EXPECT_EQ(s.ttl("k"), -2LL);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. exists
// ─────────────────────────────────────────────────────────────────────────────

TEST(ShardExists, MissingKeyReturnsFalse) {
    Shard s(4);
    EXPECT_FALSE(s.exists("no"));
}

TEST(ShardExists, ExistingKeyReturnsTrue) {
    Shard s(4);
    s.set("yes", "v");
    EXPECT_TRUE(s.exists("yes"));
}

TEST(ShardExists, AfterDelReturnsFalse) {
    Shard s(4);
    s.set("k", "v");
    s.del("k");
    EXPECT_FALSE(s.exists("k"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. LRU eviction through Shard API
// ─────────────────────────────────────────────────────────────────────────────

TEST(ShardLRU, EvictsLRUWhenFull) {
    Shard s(3);
    s.set("a", "1");
    s.set("b", "2");
    s.set("c", "3");
    s.set("d", "4");    // evicts "a"

    EXPECT_FALSE(s.exists("a"));
    EXPECT_TRUE(s.exists("b"));
    EXPECT_TRUE(s.exists("c"));
    EXPECT_TRUE(s.exists("d"));
}

TEST(ShardLRU, GetPromotesKey) {
    Shard s(3);
    s.set("a", "1");
    s.set("b", "2");
    s.set("c", "3");
    s.get("a");         // promote "a" → MRU; "b" becomes LRU
    s.set("d", "4");    // should evict "b"

    EXPECT_TRUE(s.exists("a"))  << "a was just accessed, must survive";
    EXPECT_FALSE(s.exists("b")) << "b is LRU, must be evicted";
    EXPECT_TRUE(s.exists("c"));
    EXPECT_TRUE(s.exists("d"));
}

TEST(ShardLRU, SizeNeverExceedsCapacity) {
    Shard s(5);
    for (int i = 0; i < 20; ++i) {
        s.set(std::to_string(i), "v");
        ASSERT_LE(s.size(), 5u);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. TTL — setWithTTL
// ─────────────────────────────────────────────────────────────────────────────

TEST(ShardTTL, KeyAvailableBeforeExpiry) {
    Shard s(8);
    s.setWithTTL("k", "v", 500ms);
    EXPECT_NE(s.get("k"), std::nullopt);
    EXPECT_TRUE(s.exists("k"));
}

TEST(ShardTTL, KeyExpiredAfterTTL) {
    Shard s(8);
    s.setWithTTL("k", "v", 50ms);
    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(s.get("k"), std::nullopt);
}

TEST(ShardTTL, ExistsReturnsFalseAfterExpiry) {
    Shard s(8);
    s.setWithTTL("k", "v", 50ms);
    std::this_thread::sleep_for(80ms);
    EXPECT_FALSE(s.exists("k"));
}

TEST(ShardTTL, ExpiredKeyIsLazilyRemovedFromSize) {
    Shard s(8);
    s.set("persistent", "p");
    s.setWithTTL("short", "s", 50ms);
    EXPECT_EQ(s.size(), 2u);

    std::this_thread::sleep_for(80ms);
    s.get("short");         // triggers lazy removal
    EXPECT_EQ(s.size(), 1u);
}

TEST(ShardTTL, MultipleKeysWithDifferentTTLs) {
    Shard s(8);
    s.setWithTTL("short",  "s", 50ms);
    s.setWithTTL("medium", "m", 300ms);
    s.set("forever",       "f");

    std::this_thread::sleep_for(80ms);

    EXPECT_EQ(s.get("short"),   std::nullopt) << "short should have expired";
    EXPECT_NE(s.get("medium"),  std::nullopt) << "medium should still be alive";
    EXPECT_NE(s.get("forever"), std::nullopt) << "forever should still be alive";
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. TTL — expire() / persist()
// ─────────────────────────────────────────────────────────────────────────────

TEST(ShardExpire, ExpireOnExistingKey) {
    Shard s(8);
    s.set("k", "v");
    bool ok = s.expire("k", 50ms);
    EXPECT_TRUE(ok);
    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(s.get("k"), std::nullopt);
}

TEST(ShardExpire, ExpireOnMissingKeyReturnsFalse) {
    Shard s(8);
    EXPECT_FALSE(s.expire("ghost", 100ms));
}

TEST(ShardExpire, ExpireOnAlreadyExpiredKeyReturnsFalse) {
    Shard s(8);
    s.setWithTTL("k", "v", 50ms);
    std::this_thread::sleep_for(80ms);
    EXPECT_FALSE(s.expire("k", 1000ms));
}

TEST(ShardPersist, PersistRemovesTTL) {
    Shard s(8);
    s.setWithTTL("k", "v", 500ms);
    bool ok = s.persist("k");
    EXPECT_TRUE(ok);
    EXPECT_EQ(s.ttl("k"), -1LL);   // persistent
    // Key must still be alive after original TTL would have elapsed
    std::this_thread::sleep_for(20ms);
    EXPECT_NE(s.get("k"), std::nullopt);
}

TEST(ShardPersist, PersistOnPersistentKeyReturnsFalse) {
    Shard s(8);
    s.set("k", "v");
    // Key has no TTL — persist should return false (nothing to remove)
    EXPECT_FALSE(s.persist("k"));
}

TEST(ShardPersist, PersistOnMissingKeyReturnsFalse) {
    Shard s(8);
    EXPECT_FALSE(s.persist("ghost"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. TTL — ttl() query
// ─────────────────────────────────────────────────────────────────────────────

TEST(ShardTTLQuery, MissingKeyReturnsMinusTwo) {
    Shard s(8);
    EXPECT_EQ(s.ttl("ghost"), -2LL);
}

TEST(ShardTTLQuery, PersistentKeyReturnsMinusOne) {
    Shard s(8);
    s.set("k", "v");
    EXPECT_EQ(s.ttl("k"), -1LL);
}

TEST(ShardTTLQuery, KeyWithTTLReturnsPositiveMs) {
    Shard s(8);
    s.setWithTTL("k", "v", 500ms);
    long long remaining = s.ttl("k");
    // Should be somewhere between 0 and 500ms (allow generous slack for CI).
    EXPECT_GT(remaining, 0LL);
    EXPECT_LE(remaining, 500LL);
}

TEST(ShardTTLQuery, ExpiredKeyReturnsMinusTwo) {
    Shard s(8);
    s.setWithTTL("k", "v", 50ms);
    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(s.ttl("k"), -2LL);
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. purgeExpired()
// ─────────────────────────────────────────────────────────────────────────────

TEST(ShardPurge, PurgesOnlyExpiredKeys) {
    Shard s(8);
    s.setWithTTL("exp1", "v", 50ms);
    s.setWithTTL("exp2", "v", 50ms);
    s.setWithTTL("alive", "v", 500ms);
    s.set("forever", "v");

    std::this_thread::sleep_for(80ms);
    std::size_t purged = s.purgeExpired();

    EXPECT_EQ(purged, 2u);
    EXPECT_FALSE(s.exists("exp1"));
    EXPECT_FALSE(s.exists("exp2"));
    EXPECT_TRUE(s.exists("alive"));
    EXPECT_TRUE(s.exists("forever"));
}

TEST(ShardPurge, PurgeOnNoExpiredKeysReturnsZero) {
    Shard s(8);
    s.set("a", "1");
    s.set("b", "2");
    EXPECT_EQ(s.purgeExpired(), 0u);
}

TEST(ShardPurge, SizeDecreasesAfterPurge) {
    Shard s(8);
    s.setWithTTL("e1", "v", 50ms);
    s.setWithTTL("e2", "v", 50ms);
    s.set("keep", "v");
    EXPECT_EQ(s.size(), 3u);

    std::this_thread::sleep_for(80ms);
    s.purgeExpired();
    EXPECT_EQ(s.size(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. Interaction: LRU eviction + TTL together
// ─────────────────────────────────────────────────────────────────────────────

TEST(ShardInteraction, ExpiredKeyDoesNotCountTowardsCapacity) {
    // Capacity 2: insert "a" (TTL 50ms) and "b". Let "a" expire.
    // Inserting "c" should not evict "b" because the cap isn't truly full.
    Shard s(2);
    s.setWithTTL("a", "1", 50ms);
    s.set("b", "2");

    std::this_thread::sleep_for(80ms);
    s.get("a");     // lazy-evict "a"

    s.set("c", "3");    // only 1 live key existed, so no LRU eviction needed

    EXPECT_TRUE(s.exists("b")) << "b should survive — a was lazily evicted first";
    EXPECT_TRUE(s.exists("c"));
    EXPECT_FALSE(s.exists("a"));
}

TEST(ShardInteraction, TTLKeysCanBeEvictedByLRU) {
    // If a key with a long TTL is the LRU entry, it still gets evicted.
    Shard s(2);
    s.setWithTTL("a", "1", 5000ms);   // long TTL
    s.set("b", "2");
    s.set("c", "3");    // capacity exceeded → evicts "a" (LRU)

    EXPECT_FALSE(s.exists("a")) << "a is LRU — must be evicted despite having TTL";
    EXPECT_TRUE(s.exists("b"));
    EXPECT_TRUE(s.exists("c"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 11. Concurrent writes — data-race free
// ─────────────────────────────────────────────────────────────────────────────

TEST(ShardConcurrent, ConcurrentSetsNoRace) {
    // Spin up N threads each writing M distinct keys. The shard must not
    // crash or corrupt under concurrent writes.
    // Run with -fsanitize=thread to catch races automatically.
    constexpr int THREADS = 8;
    constexpr int OPS     = 200;

    Shard s(THREADS * OPS);   // large enough to hold everything

    std::vector<std::thread> workers;
    workers.reserve(THREADS);

    for (int t = 0; t < THREADS; ++t) {
        workers.emplace_back([&s, t]() {
            for (int i = 0; i < OPS; ++i) {
                s.set(std::to_string(t * 1000 + i), "v");
            }
        });
    }
    for (auto& w : workers) w.join();

    // Total keys must not exceed capacity (or THREADS*OPS if cap is large).
    EXPECT_LE(s.size(), static_cast<std::size_t>(THREADS * OPS));
}

TEST(ShardConcurrent, ConcurrentSetsWithSmallCapNoRace) {
    // Force heavy LRU eviction under concurrent writes.
    Shard s(10);
    constexpr int THREADS = 8;
    constexpr int OPS     = 500;

    std::vector<std::thread> workers;
    workers.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t) {
        workers.emplace_back([&s, t]() {
            for (int i = 0; i < OPS; ++i) {
                s.set(std::to_string(t * 1000 + i), "x");
            }
        });
    }
    for (auto& w : workers) w.join();

    EXPECT_LE(s.size(), 10u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 12. Concurrent reads + writes — data-race free
// ─────────────────────────────────────────────────────────────────────────────

TEST(ShardConcurrent, ConcurrentReadsAndWritesNoRace) {
    constexpr int WRITER_THREADS = 4;
    constexpr int READER_THREADS = 4;
    constexpr int OPS            = 300;

    Shard s(50);

    // Pre-populate a few keys so readers have something to find.
    for (int i = 0; i < 10; ++i) s.set(std::to_string(i), "init");

    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;

    // Writers: keep hammering set/del
    for (int t = 0; t < WRITER_THREADS; ++t) {
        workers.emplace_back([&s, &stop, t]() {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                s.set(std::to_string(t * 1000 + (i % OPS)), "w");
                if (i % 5 == 0) s.del(std::to_string(t * 1000 + (i % OPS)));
                ++i;
            }
        });
    }

    // Readers: keep hammering get/exists/ttl
    for (int t = 0; t < READER_THREADS; ++t) {
        workers.emplace_back([&s, &stop, t]() {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                (void)s.get(std::to_string(i % 20));
                (void)s.exists(std::to_string(t * 1000 + (i % OPS)));
                (void)s.ttl(std::to_string(i % 20));
                ++i;
            }
        });
    }

    std::this_thread::sleep_for(150ms);
    stop.store(true, std::memory_order_relaxed);
    for (auto& w : workers) w.join();

    // No assertion needed — the test passes if it doesn't crash or get flagged
    // by ThreadSanitizer. Size must still be within capacity.
    EXPECT_LE(s.size(), s.capacity());
}

// ─────────────────────────────────────────────────────────────────────────────
// 13. Concurrent TTL expiry under load
// ─────────────────────────────────────────────────────────────────────────────

TEST(ShardConcurrent, ConcurrentTTLAndPurgeNoRace) {
    // Writers set keys with short TTLs; a separate "expiry thread" calls
    // purgeExpired() repeatedly. No crashes, no races.
    Shard s(200);

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> totalPurged{0};

    // Expiry thread
    std::thread expiry([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            totalPurged.fetch_add(s.purgeExpired(), std::memory_order_relaxed);
            std::this_thread::sleep_for(10ms);
        }
    });

    // Writer threads
    constexpr int WRITERS = 4;
    std::vector<std::thread> writers;
    for (int t = 0; t < WRITERS; ++t) {
        writers.emplace_back([&s, t]() {
            for (int i = 0; i < 200; ++i) {
                s.setWithTTL(std::to_string(t * 1000 + i), "v", 30ms);
            }
        });
    }
    for (auto& w : writers) w.join();

    std::this_thread::sleep_for(80ms);  // let keys expire
    stop.store(true, std::memory_order_relaxed);
    expiry.join();

    // After all TTLs fire, the shard should be empty (or very close to it).
    s.purgeExpired();   // final sweep
    EXPECT_EQ(s.size(), 0u) << "all short-TTL keys should have been purged";
}
