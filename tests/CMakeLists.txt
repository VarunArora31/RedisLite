enable_testing()

# ── Eviction (LRUCache) tests ─────────────────────────────────────────────────
add_executable(test_eviction
    test_eviction.cpp
)

target_link_libraries(test_eviction
    PRIVATE
        GTest::gtest_main   # provides main(); no need to write our own
)

# Make ctest aware of all Google Test cases inside this binary
include(GoogleTest)
gtest_discover_tests(test_eviction)

# ── Future test targets go here ───────────────────────────────────────────────
add_executable(test_shard
    test_shard.cpp
)

target_link_libraries(test_shard
    PRIVATE
        GTest::gtest_main
)

gtest_discover_tests(test_shard)

# add_executable(test_cache       test_cache.cpp)
# target_link_libraries(test_cache PRIVATE GTest::gtest_main)
# gtest_discover_tests(test_cache)
#
# add_executable(test_protocol    test_protocol.cpp)
# target_link_libraries(test_protocol PRIVATE GTest::gtest_main)
# gtest_discover_tests(test_protocol)
