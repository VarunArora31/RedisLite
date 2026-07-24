// tests/test_expiry.cpp
//
// Tests for cache::ExpiryWorker (src/expiry.hpp).
//
// Test groups:
//   1.  Construction — valid / invalid interval
//   2.  Lifecycle — start / stop / isRunning
//   3.  Idempotency — double start, double stop
//   4.  RAII — destructor stops thread cleanly
//   5.  Active expiry — worker actually purges expired keys
//   6.  Non-expired keys are not purged
//   7.  totalPurged() counter
//   8.  Interval respected — sweep frequency
//   9.  Stop is prompt — wakes before interval expires

#include "expiry.hpp"
#include "cache.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>

using cache::ExpiryWorker;
using cache::ShardedCache;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// 1. Construction
// ─────────────────────────────────────────────────────────────────────────────

TEST(ExpiryConstruction, ValidIntervalCreated) {
    ShardedCache c(128);
    EXPECT_NO_THROW({ ExpiryWorker w(c, 50ms); (void)w; });
}

TEST(ExpiryConstruction, ZeroIntervalThrows) {
    ShardedCache c(128);
    using Ms = std::chrono::milliseconds;
    EXPECT_THROW({ ExpiryWorker w(c, Ms{0}); (void)w; }, std::invalid_argument);
}

TEST(ExpiryConstruction, NegativeIntervalThrows) {
    ShardedCache c(128);
    using Ms = std::chrono::milliseconds;
    EXPECT_THROW({ ExpiryWorker w(c, Ms{-1}); (void)w; }, std::invalid_argument);
}

TEST(ExpiryConstruction, NotRunningAfterConstruct) {
    ShardedCache c(128);
    ExpiryWorker w(c, 100ms);
    EXPECT_FALSE(w.isRunning());
}

TEST(ExpiryConstruction, IntervalReported) {
    ShardedCache c(128);
    ExpiryWorker w(c, 77ms);
    EXPECT_EQ(w.interval(), 77ms);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Lifecycle — start / stop / isRunning
// ─────────────────────────────────────────────────────────────────────────────

TEST(ExpiryLifecycle, StartSetsIsRunning) {
    ShardedCache c(128);
    ExpiryWorker w(c, 200ms);
    w.start();
    EXPECT_TRUE(w.isRunning());
    w.stop();
}

TEST(ExpiryLifecycle, StopClearsIsRunning) {
    ShardedCache c(128);
    ExpiryWorker w(c, 200ms);
    w.start();
    w.stop();
    EXPECT_FALSE(w.isRunning());
}

TEST(ExpiryLifecycle, StopWithoutStartIsNoOp) {
    ShardedCache c(128);
    ExpiryWorker w(c, 100ms);
    EXPECT_NO_THROW(w.stop());
    EXPECT_FALSE(w.isRunning());
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Idempotency
// ─────────────────────────────────────────────────────────────────────────────

TEST(ExpiryIdempotency, DoubleStartIsNoOp) {
    ShardedCache c(128);
    ExpiryWorker w(c, 200ms);
    w.start();
    EXPECT_NO_THROW(w.start());  // second start must not crash
    EXPECT_TRUE(w.isRunning());
    w.stop();
}

TEST(ExpiryIdempotency, DoubleStopIsNoOp) {
    ShardedCache c(128);
    ExpiryWorker w(c, 200ms);
    w.start();
    w.stop();
    EXPECT_NO_THROW(w.stop());  // second stop must not crash
    EXPECT_FALSE(w.isRunning());
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. RAII — destructor stops thread cleanly
// ─────────────────────────────────────────────────────────────────────────────

TEST(ExpiryRAII, DestructorStopsThread) {
    ShardedCache c(128);
    {
        ExpiryWorker w(c, 200ms);
        w.start();
        EXPECT_TRUE(w.isRunning());
        // w goes out of scope here — destructor must join cleanly
    }
    // If we reach here without hanging, the thread joined correctly.
    SUCCEED();
}

TEST(ExpiryRAII, DestructorWithoutStartIsClean) {
    ShardedCache c(128);
    { ExpiryWorker w(c, 100ms); }   // never started — destructor must be a no-op
    SUCCEED();
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Active expiry — worker actually purges expired keys
// ─────────────────────────────────────────────────────────────────────────────

TEST(ExpiryActive, WorkerPurgesExpiredKeys) {
    ShardedCache c(256, 4);

    // Set 10 keys with 50ms TTL
    for (int i = 0; i < 10; ++i) {
        c.setWithTTL("k" + std::to_string(i), "v", 50ms);
    }
    EXPECT_EQ(c.size(), 10u);

    ExpiryWorker w(c, 30ms);   // sweep every 30ms
    w.start();

    // Wait long enough for at least two sweeps after keys expire
    std::this_thread::sleep_for(200ms);
    w.stop();

    // All expired keys should have been actively purged
    EXPECT_EQ(c.size(), 0u) << "worker should have purged all expired keys";
}

TEST(ExpiryActive, WorkerPurgesOnlyExpiredKeys) {
    ShardedCache c(256, 4);

    for (int i = 0; i < 5; ++i) {
        c.setWithTTL("exp_" + std::to_string(i), "v", 50ms);
    }
    for (int i = 0; i < 5; ++i) {
        c.set("keep_" + std::to_string(i), "v");
    }
    EXPECT_EQ(c.size(), 10u);

    ExpiryWorker w(c, 30ms);
    w.start();
    std::this_thread::sleep_for(200ms);
    w.stop();

    EXPECT_EQ(c.size(), 5u) << "only expired keys should be gone";
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(c.exists("keep_" + std::to_string(i)));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Non-expired keys are not purged
// ─────────────────────────────────────────────────────────────────────────────

TEST(ExpiryActive, LongTTLKeysNotPurged) {
    ShardedCache c(64);
    c.setWithTTL("long", "v", 10000ms);   // 10 seconds — won't expire in test
    c.set("persistent", "v");

    ExpiryWorker w(c, 30ms);
    w.start();
    std::this_thread::sleep_for(100ms);
    w.stop();

    EXPECT_TRUE(c.exists("long"));
    EXPECT_TRUE(c.exists("persistent"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. totalPurged() counter
// ─────────────────────────────────────────────────────────────────────────────

TEST(ExpiryCounter, TotalPurgedStartsAtZero) {
    ShardedCache c(64);
    ExpiryWorker w(c, 100ms);
    EXPECT_EQ(w.totalPurged(), 0u);
}

TEST(ExpiryCounter, TotalPurgedCountsRemovedKeys) {
    ShardedCache c(256, 4);
    for (int i = 0; i < 8; ++i) {
        c.setWithTTL("k" + std::to_string(i), "v", 50ms);
    }

    ExpiryWorker w(c, 30ms);
    w.start();
    std::this_thread::sleep_for(200ms);
    w.stop();

    EXPECT_GE(w.totalPurged(), 8u) << "should have purged all 8 expired keys";
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. Stop is prompt — wakes before interval expires
// ─────────────────────────────────────────────────────────────────────────────

TEST(ExpiryLifecycle, StopReturnsQuickly) {
    ShardedCache c(64);
    // Very long interval — without cv wakeup, stop() would block for 10s
    ExpiryWorker w(c, 10000ms);
    w.start();

    auto t0 = std::chrono::steady_clock::now();
    w.stop();
    auto elapsed = std::chrono::steady_clock::now() - t0;

    // Should return well under 1 second
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
              1000LL)
        << "stop() should wake the thread immediately via condition_variable";
}
