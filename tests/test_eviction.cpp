// tests/test_eviction.cpp
//
// Unit tests for cache::LRUCache (src/eviction.hpp).
//
// Test groups:
//   1. Construction & capacity
//   2. Basic put / get
//   3. LRU eviction ordering
//   4. Update-in-place (put on existing key)
//   5. contains() / size() invariants
//   6. get() promotes to MRU (access-order correctness)
//   7. Capacity-1 edge cases
//   8. Overwrite does NOT count as a new entry (no eviction)
//   9. Move semantics
//  10. Non-trivial value types (move-only, string)

#include "eviction.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>

using cache::LRUCache;

// ─────────────────────────────────────────────────────────────────────────────
// 1. Construction & capacity
// ─────────────────────────────────────────────────────────────────────────────

TEST(Construction, ZeroCapacityThrows) {
    // Use a type alias so the template comma doesn't confuse the macro parser.
    using C = LRUCache<std::string, int>;
    EXPECT_THROW(C{0}, std::invalid_argument);
}

TEST(Construction, CapacityOneIsValid) {
    using C = LRUCache<std::string, int>;
    EXPECT_NO_THROW(C{1});
}

TEST(Construction, CapacityReportedCorrectly) {
    LRUCache<std::string, int> c(42);
    EXPECT_EQ(c.capacity(), 42u);
}

TEST(Construction, StartsEmpty) {
    LRUCache<std::string, int> c(10);
    EXPECT_EQ(c.size(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Basic put / get
// ─────────────────────────────────────────────────────────────────────────────

TEST(PutGet, GetMissingKeyReturnsNullopt) {
    LRUCache<std::string, int> c(4);
    EXPECT_EQ(c.get("missing"), std::nullopt);
}

TEST(PutGet, GetAfterPutReturnsValue) {
    LRUCache<std::string, int> c(4);
    c.put("a", 1);
    ASSERT_NE(c.get("a"), std::nullopt);
    EXPECT_EQ(*c.get("a"), 1);
}

TEST(PutGet, MultipleDistinctKeys) {
    LRUCache<std::string, int> c(4);
    c.put("x", 10);
    c.put("y", 20);
    c.put("z", 30);

    EXPECT_EQ(*c.get("x"), 10);
    EXPECT_EQ(*c.get("y"), 20);
    EXPECT_EQ(*c.get("z"), 30);
}

TEST(PutGet, SizeTracksInsertions) {
    LRUCache<std::string, int> c(5);
    EXPECT_EQ(c.size(), 0u);
    c.put("a", 1);
    EXPECT_EQ(c.size(), 1u);
    c.put("b", 2);
    EXPECT_EQ(c.size(), 2u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. LRU eviction ordering
// ─────────────────────────────────────────────────────────────────────────────

TEST(Eviction, LeastRecentlyUsedIsEvictedFirst) {
    // Insertion order: a, b, c  — at capacity (3).
    // Adding d must evict a (the oldest, never touched again).
    LRUCache<std::string, int> c(3);
    c.put("a", 1);
    c.put("b", 2);
    c.put("c", 3);

    c.put("d", 4); // evicts "a"

    EXPECT_EQ(c.get("a"), std::nullopt) << "a should have been evicted";
    EXPECT_NE(c.get("b"), std::nullopt);
    EXPECT_NE(c.get("c"), std::nullopt);
    EXPECT_NE(c.get("d"), std::nullopt);
}

TEST(Eviction, SizeNeverExceedsCapacity) {
    LRUCache<std::string, int> c(3);
    c.put("a", 1);
    c.put("b", 2);
    c.put("c", 3);
    c.put("d", 4);
    c.put("e", 5);

    EXPECT_EQ(c.size(), 3u);
    EXPECT_LE(c.size(), c.capacity());
}

TEST(Eviction, EvictionSequenceIsCorrect) {
    // Insert 1,2,3 into a cap-3 cache (no gets between inserts — gets would
    // promote keys to MRU and change the eviction order, which is tested
    // separately in AccessOrder tests).
    // Pure insertion order: LRU order is 1 < 2 < 3 (1 is oldest).
    LRUCache<int, int> c(3);
    c.put(1, 10);
    c.put(2, 20);
    c.put(3, 30);

    // Insert 4 → evicts 1 (LRU).  Now cache holds {2,3,4}.
    c.put(4, 40);

    // Insert 5 → evicts 2 (LRU).  Now cache holds {3,4,5}.
    c.put(5, 50);

    // Insert 6 → evicts 3 (LRU).  Now cache holds {4,5,6}.
    c.put(6, 60);

    // Assert final state — do all checks at the end, no mid-sequence gets.
    EXPECT_EQ(c.get(1), std::nullopt) << "1 should have been evicted by insert 4";
    EXPECT_EQ(c.get(2), std::nullopt) << "2 should have been evicted by insert 5";
    EXPECT_EQ(c.get(3), std::nullopt) << "3 should have been evicted by insert 6";
    EXPECT_NE(c.get(4), std::nullopt) << "4 should still be present";
    EXPECT_NE(c.get(5), std::nullopt) << "5 should still be present";
    EXPECT_NE(c.get(6), std::nullopt) << "6 should still be present";
}

TEST(Eviction, EvictedKeyIsFullyRemovedFromContains) {
    LRUCache<std::string, int> c(2);
    c.put("a", 1);
    c.put("b", 2);
    c.put("c", 3); // evicts "a"

    EXPECT_FALSE(c.contains("a"));
    EXPECT_TRUE(c.contains("b"));
    EXPECT_TRUE(c.contains("c"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Update-in-place (put on an existing key)
// ─────────────────────────────────────────────────────────────────────────────

TEST(UpdateInPlace, OverwriteUpdatesValue) {
    LRUCache<std::string, int> c(4);
    c.put("k", 1);
    c.put("k", 99);
    EXPECT_EQ(*c.get("k"), 99);
}

TEST(UpdateInPlace, OverwriteDoesNotGrowSize) {
    LRUCache<std::string, int> c(4);
    c.put("k", 1);
    c.put("k", 2);
    EXPECT_EQ(c.size(), 1u);
}

TEST(UpdateInPlace, OverwriteDoesNotTriggerEviction) {
    // Fill to capacity, then overwrite an existing key.
    // No entry should be evicted because size hasn't grown.
    LRUCache<std::string, int> c(3);
    c.put("a", 1);
    c.put("b", 2);
    c.put("c", 3);

    c.put("b", 200); // update, not insert

    EXPECT_EQ(c.size(), 3u);
    EXPECT_TRUE(c.contains("a"));
    EXPECT_TRUE(c.contains("b"));
    EXPECT_TRUE(c.contains("c"));
    EXPECT_EQ(*c.get("b"), 200);
}

TEST(UpdateInPlace, OverwritePromotesToMRU) {
    // a, b, c inserted; b is overwritten → b becomes MRU.
    // Next insert should evict a (LRU), not b.
    LRUCache<std::string, int> c(3);
    c.put("a", 1);
    c.put("b", 2);
    c.put("c", 3);

    c.put("b", 99); // promotes b to MRU

    c.put("d", 4); // at capacity+1; should evict a (LRU), not b

    EXPECT_EQ(c.get("a"), std::nullopt) << "a should be evicted (it is LRU)";
    EXPECT_TRUE(c.contains("b"))        << "b should survive (it was just updated)";
    EXPECT_TRUE(c.contains("c"));
    EXPECT_TRUE(c.contains("d"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. contains() / size() invariants
// ─────────────────────────────────────────────────────────────────────────────

TEST(ContainsSize, ContainsReturnsFalseForMissingKey) {
    LRUCache<std::string, int> c(4);
    EXPECT_FALSE(c.contains("ghost"));
}

TEST(ContainsSize, ContainsReturnsTrueAfterPut) {
    LRUCache<std::string, int> c(4);
    c.put("key", 42);
    EXPECT_TRUE(c.contains("key"));
}

TEST(ContainsSize, SizeAndContainsStayConsistent) {
    LRUCache<std::string, int> c(3);
    c.put("a", 1);
    c.put("b", 2);
    c.put("c", 3);
    c.put("d", 4); // evicts a

    // Exactly 3 keys should exist: b, c, d
    EXPECT_EQ(c.size(), 3u);
    EXPECT_FALSE(c.contains("a"));
    EXPECT_TRUE(c.contains("b"));
    EXPECT_TRUE(c.contains("c"));
    EXPECT_TRUE(c.contains("d"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. get() promotes to MRU (access-order correctness)
// ─────────────────────────────────────────────────────────────────────────────

TEST(AccessOrder, GetPromotesKeyToMRU) {
    // a inserted first (LRU candidate), then b and c.
    // We GET a → a becomes MRU. Next insert should evict b, not a.
    LRUCache<std::string, int> c(3);
    c.put("a", 1);
    c.put("b", 2);
    c.put("c", 3);

    c.get("a"); // promote a to MRU

    c.put("d", 4); // should evict b (now the LRU)

    EXPECT_TRUE(c.contains("a"))        << "a was just accessed, should survive";
    EXPECT_EQ(c.get("b"), std::nullopt) << "b should be evicted";
    EXPECT_TRUE(c.contains("c"));
    EXPECT_TRUE(c.contains("d"));
}

TEST(AccessOrder, MultipleGetsShiftLRUCorrectly) {
    LRUCache<int, int> c(3);
    c.put(1, 10);
    c.put(2, 20);
    c.put(3, 30);

    // Access order from least recent: 1 → 2 → 3
    // Now access 1 and 2, making 3 the LRU
    c.get(1);
    c.get(2);

    c.put(4, 40); // evicts 3

    EXPECT_EQ(c.get(3), std::nullopt);
    EXPECT_NE(c.get(1), std::nullopt);
    EXPECT_NE(c.get(2), std::nullopt);
    EXPECT_NE(c.get(4), std::nullopt);
}

TEST(AccessOrder, GetMissingKeyDoesNotAffectOrder) {
    LRUCache<std::string, int> c(2);
    c.put("a", 1);
    c.put("b", 2);

    c.get("ghost"); // miss — should not disturb internal order

    c.put("c", 3); // should evict "a" (LRU), not "b"

    EXPECT_EQ(c.get("a"), std::nullopt);
    EXPECT_TRUE(c.contains("b"));
    EXPECT_TRUE(c.contains("c"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. Capacity-1 edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(CapacityOne, PutAndGetWork) {
    LRUCache<std::string, int> c(1);
    c.put("only", 7);
    EXPECT_EQ(*c.get("only"), 7);
}

TEST(CapacityOne, NewInsertEvictsPreviousKey) {
    LRUCache<std::string, int> c(1);
    c.put("first", 1);
    c.put("second", 2);

    EXPECT_EQ(c.get("first"), std::nullopt);
    EXPECT_EQ(*c.get("second"), 2);
    EXPECT_EQ(c.size(), 1u);
}

TEST(CapacityOne, OverwriteDoesNotEvict) {
    LRUCache<std::string, int> c(1);
    c.put("k", 1);
    c.put("k", 2);

    EXPECT_EQ(*c.get("k"), 2);
    EXPECT_EQ(c.size(), 1u);
}

TEST(CapacityOne, RepeatedReplacementKeepsSizeAtOne) {
    LRUCache<int, int> c(1);
    for (int i = 0; i < 100; ++i) {
        c.put(i, i * 10);
        EXPECT_EQ(c.size(), 1u);
        EXPECT_LE(c.size(), c.capacity());
    }
    // Only the last key should exist
    EXPECT_EQ(*c.get(99), 990);
    EXPECT_EQ(c.get(0), std::nullopt);
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. Large-scale consistency
// ─────────────────────────────────────────────────────────────────────────────

TEST(LargeScale, SizeNeverExceedsCapacityUnderLoad) {
    constexpr std::size_t CAP = 50;
    LRUCache<int, int> c(CAP);

    for (int i = 0; i < 1000; ++i) {
        c.put(i, i);
        ASSERT_LE(c.size(), CAP) << "size exceeded capacity after inserting key " << i;
    }
}

TEST(LargeScale, ContainsAndGetAgreeOnEviction) {
    // After filling past capacity, contains() and get() must agree
    // (both return false/nullopt for the same keys).
    constexpr std::size_t CAP = 10;
    LRUCache<int, int> c(CAP);

    for (int i = 0; i < 20; ++i) {
        c.put(i, i * 2);
    }

    for (int i = 0; i < 20; ++i) {
        bool inCache   = c.contains(i);
        bool getExists = c.get(i).has_value();
        EXPECT_EQ(inCache, getExists)
            << "contains() and get() disagree for key " << i;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. Move semantics
// ─────────────────────────────────────────────────────────────────────────────

TEST(MoveSemantics, MoveConstructedCacheIsUsable) {
    LRUCache<std::string, int> original(3);
    original.put("a", 1);
    original.put("b", 2);

    LRUCache<std::string, int> moved(std::move(original));

    EXPECT_EQ(*moved.get("a"), 1);
    EXPECT_EQ(*moved.get("b"), 2);
    EXPECT_EQ(moved.size(), 2u);
    EXPECT_EQ(moved.capacity(), 3u);
}

TEST(MoveSemantics, MoveAssignedCacheIsUsable) {
    LRUCache<std::string, int> src(2);
    src.put("x", 10);

    LRUCache<std::string, int> dst(5);
    dst = std::move(src);

    EXPECT_EQ(*dst.get("x"), 10);
    EXPECT_EQ(dst.capacity(), 2u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. Non-trivial value types
// ─────────────────────────────────────────────────────────────────────────────

TEST(ValueTypes, StringValues) {
    LRUCache<std::string, std::string> c(3);
    c.put("greeting", "hello");
    c.put("farewell", "goodbye");

    EXPECT_EQ(*c.get("greeting"), "hello");
    EXPECT_EQ(*c.get("farewell"), "goodbye");
}

TEST(ValueTypes, StringValuesEvictCorrectly) {
    LRUCache<std::string, std::string> c(2);
    c.put("a", "alpha");
    c.put("b", "beta");
    c.put("c", "gamma"); // evicts "a"

    EXPECT_EQ(c.get("a"), std::nullopt);
    EXPECT_EQ(*c.get("b"), "beta");
    EXPECT_EQ(*c.get("c"), "gamma");
}

TEST(ValueTypes, IntegerKeysWork) {
    LRUCache<int, std::string> c(3);
    c.put(1, "one");
    c.put(2, "two");
    c.put(3, "three");

    EXPECT_EQ(*c.get(1), "one");
    EXPECT_EQ(*c.get(2), "two");
    EXPECT_EQ(*c.get(3), "three");
}
