// tests/test_persistence.cpp
//
// Tests for persistence::save() and persistence::load() (src/persistence.hpp).
//
// Test groups:
//   1.  save() basic — file is created
//   2.  load() basic — missing file throws
//   3.  Round-trip — persistent keys survive save/load
//   4.  Round-trip — TTL keys survive save/load with remaining TTL
//   5.  Round-trip — expired keys are NOT loaded back
//   6.  Corrupt / malformed file — load throws
//   7.  Empty cache — save/load works with zero entries
//   8.  Large snapshot — many keys round-trip correctly
//   9.  Binary-safe values — keys/values with spaces and special chars
//  10.  Overwrite — saving twice replaces the previous snapshot

#include "persistence.hpp"
#include "cache.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

using cache::ShardedCache;
using namespace std::chrono_literals;

// ── Helper: temp file path ────────────────────────────────────────────────────
// Each test gets its own path so parallel runs don't collide.
static std::string tmpPath(const std::string& name) {
    return std::string("test_snap_") + name + ".rdb";
}

// RAII cleanup: delete the snapshot file when the guard goes out of scope.
struct FileGuard {
    std::string path;
    explicit FileGuard(std::string p) : path(std::move(p)) {}
    ~FileGuard() { std::remove(path.c_str()); }
};

// ─────────────────────────────────────────────────────────────────────────────
// 1. save() basic
// ─────────────────────────────────────────────────────────────────────────────

TEST(PersistenceSave, FileIsCreated) {
    std::string path = tmpPath("created");
    FileGuard   guard(path);

    ShardedCache c(64);
    c.set("k", "v");
    EXPECT_NO_THROW(persistence::save(c, path));

    std::ifstream f(path);
    EXPECT_TRUE(f.good()) << "snapshot file should exist after save()";
}

TEST(PersistenceSave, EmptyCacheSavesWithoutError) {
    std::string path = tmpPath("empty_save");
    FileGuard   guard(path);

    ShardedCache c(64);
    EXPECT_NO_THROW(persistence::save(c, path));
}

TEST(PersistenceSave, SaveToBadPathThrows) {
    ShardedCache c(64);
    c.set("k", "v");
    EXPECT_THROW(persistence::save(c, "/nonexistent/dir/snap.rdb"),
                 std::runtime_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. load() basic
// ─────────────────────────────────────────────────────────────────────────────

TEST(PersistenceLoad, MissingFileThrows) {
    ShardedCache c(64);
    EXPECT_THROW(persistence::load(c, "does_not_exist.rdb"),
                 std::runtime_error);
}

TEST(PersistenceLoad, EmptyFileThrows) {
    std::string path = tmpPath("empty_load");
    FileGuard   guard(path);

    // Create an empty file
    std::ofstream(path).close();
    ShardedCache c(64);
    EXPECT_THROW(persistence::load(c, path), std::runtime_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Round-trip — persistent keys
// ─────────────────────────────────────────────────────────────────────────────

TEST(PersistenceRoundTrip, PersistentKeysSurvive) {
    std::string path = tmpPath("persistent");
    FileGuard   guard(path);

    // Save
    {
        ShardedCache src(64);
        src.set("alpha", "one");
        src.set("beta",  "two");
        src.set("gamma", "three");
        persistence::save(src, path);
    }

    // Load into fresh cache
    ShardedCache dst(64);
    std::size_t n = persistence::load(dst, path);

    EXPECT_EQ(n, 3u);
    EXPECT_EQ(*dst.get("alpha"), "one");
    EXPECT_EQ(*dst.get("beta"),  "two");
    EXPECT_EQ(*dst.get("gamma"), "three");
}

TEST(PersistenceRoundTrip, LoadedKeyIsPersistent) {
    std::string path = tmpPath("persistent_ttl");
    FileGuard   guard(path);

    {
        ShardedCache src(64);
        src.set("k", "v");
        persistence::save(src, path);
    }

    ShardedCache dst(64);
    persistence::load(dst, path);

    // TTL should be -1 (persistent), not -2 (missing)
    EXPECT_EQ(dst.ttl("k"), -1LL);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Round-trip — keys with TTL
// ─────────────────────────────────────────────────────────────────────────────

TEST(PersistenceRoundTrip, TTLKeysSurviveWithRemainingTTL) {
    std::string path = tmpPath("ttl");
    FileGuard   guard(path);

    {
        ShardedCache src(64);
        src.setWithTTL("expiring", "val", 5000ms);  // 5 second TTL
        persistence::save(src, path);
    }

    ShardedCache dst(64);
    persistence::load(dst, path);

    // Key should exist
    ASSERT_NE(dst.get("expiring"), std::nullopt);

    // TTL should be positive and <= 5000
    long long remaining = dst.ttl("expiring");
    EXPECT_GT(remaining, 0LL);
    EXPECT_LE(remaining, 5000LL);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Expired keys are NOT loaded back
// ─────────────────────────────────────────────────────────────────────────────

TEST(PersistenceRoundTrip, ExpiredKeyNotReloaded) {
    std::string path = tmpPath("expired");
    FileGuard   guard(path);

    // Set a key with 50ms TTL, wait for it to fully expire, THEN save.
    // The snapshot will either skip it (ttlMs==0) or not include it at all.
    ShardedCache src(64);
    src.setWithTTL("gone", "v", 50ms);
    src.set("alive", "v");

    // Wait for "gone" to expire before snapshotting
    std::this_thread::sleep_for(100ms);
    src.purgeExpired();   // actively remove it so snapshot won't see it

    persistence::save(src, path);

    ShardedCache dst(64);
    std::size_t n = persistence::load(dst, path);

    EXPECT_FALSE(dst.exists("gone"))   << "expired key should not be reloaded";
    EXPECT_TRUE(dst.exists("alive"))   << "persistent key should survive";
    EXPECT_EQ(n, 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Corrupt / malformed files
// ─────────────────────────────────────────────────────────────────────────────

TEST(PersistenceCorrupt, WrongMagicThrows) {
    std::string path = tmpPath("bad_magic");
    FileGuard   guard(path);

    std::ofstream(path) << "NOT-A-SNAPSHOT v1\n";
    ShardedCache c(64);
    EXPECT_THROW(persistence::load(c, path), std::runtime_error);
}

TEST(PersistenceCorrupt, WrongVersionThrows) {
    std::string path = tmpPath("bad_version");
    FileGuard   guard(path);

    std::ofstream(path) << "REDISLITE-SNAPSHOT v99\n";
    ShardedCache c(64);
    EXPECT_THROW(persistence::load(c, path), std::runtime_error);
}

TEST(PersistenceCorrupt, MalformedEntryLineThrows) {
    std::string path = tmpPath("bad_entry");
    FileGuard   guard(path);

    std::ofstream(path) << "REDISLITE-SNAPSHOT v1\n"
                        << "notanumber\n";
    ShardedCache c(64);
    EXPECT_THROW(persistence::load(c, path), std::runtime_error);
}

TEST(PersistenceCorrupt, TruncatedKeyThrows) {
    std::string path = tmpPath("truncated_key");
    FileGuard   guard(path);

    // Header says key is 10 bytes but only 3 are written
    std::ofstream(path) << "REDISLITE-SNAPSHOT v1\n"
                        << "10 5 -1\n"
                        << "abc";   // only 3 bytes, expected 10
    ShardedCache c(64);
    EXPECT_THROW(persistence::load(c, path), std::runtime_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. Empty cache
// ─────────────────────────────────────────────────────────────────────────────

TEST(PersistenceRoundTrip, EmptyCacheRoundTrip) {
    std::string path = tmpPath("empty_rt");
    FileGuard   guard(path);

    {
        ShardedCache src(64);
        persistence::save(src, path);
    }

    ShardedCache dst(64);
    std::size_t n = persistence::load(dst, path);
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(dst.size(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. Large snapshot
// ─────────────────────────────────────────────────────────────────────────────

TEST(PersistenceRoundTrip, LargeSnapshotRoundTrips) {
    std::string path = tmpPath("large");
    FileGuard   guard(path);

    constexpr int N = 500;

    {
        ShardedCache src(N * 2, 16);
        for (int i = 0; i < N; ++i) {
            src.set("key_" + std::to_string(i),
                    "value_" + std::to_string(i));
        }
        persistence::save(src, path);
    }

    ShardedCache dst(N * 2, 16);
    std::size_t n = persistence::load(dst, path);

    EXPECT_EQ(n, static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) {
        ASSERT_NE(dst.get("key_" + std::to_string(i)), std::nullopt)
            << "missing key_" << i;
        EXPECT_EQ(*dst.get("key_" + std::to_string(i)),
                  "value_" + std::to_string(i));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. Binary-safe values
// ─────────────────────────────────────────────────────────────────────────────

TEST(PersistenceRoundTrip, ValueWithSpacesSurvives) {
    std::string path = tmpPath("spaces");
    FileGuard   guard(path);

    {
        ShardedCache src(64);
        src.set("greeting", "hello world");
        persistence::save(src, path);
    }

    ShardedCache dst(64);
    persistence::load(dst, path);
    EXPECT_EQ(*dst.get("greeting"), "hello world");
}

TEST(PersistenceRoundTrip, KeyWithSpecialCharsSurvives) {
    std::string path = tmpPath("special");
    FileGuard   guard(path);

    {
        ShardedCache src(64);
        src.set("user:1:name",  "Alice");
        src.set("user:1:score", "42");
        persistence::save(src, path);
    }

    ShardedCache dst(64);
    persistence::load(dst, path);
    EXPECT_EQ(*dst.get("user:1:name"),  "Alice");
    EXPECT_EQ(*dst.get("user:1:score"), "42");
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. Overwrite — saving twice replaces the previous snapshot
// ─────────────────────────────────────────────────────────────────────────────

TEST(PersistenceRoundTrip, SecondSaveOverwritesFirst) {
    std::string path = tmpPath("overwrite");
    FileGuard   guard(path);

    ShardedCache src(64);

    // First save: key "old"
    src.set("old", "value");
    persistence::save(src, path);

    // Second save: key "new" only
    src.del("old");
    src.set("new_key", "new_value");
    persistence::save(src, path);

    // Load should reflect the second save only
    ShardedCache dst(64);
    persistence::load(dst, path);

    EXPECT_FALSE(dst.exists("old"))     << "old key should be gone";
    EXPECT_TRUE(dst.exists("new_key"))  << "new key should be present";
    EXPECT_EQ(*dst.get("new_key"), "new_value");
}
