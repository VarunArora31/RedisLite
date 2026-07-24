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

add_executable(test_cache
    test_cache.cpp
)

target_link_libraries(test_cache
    PRIVATE
        GTest::gtest_main
)

gtest_discover_tests(test_cache)

add_executable(test_protocol
    test_protocol.cpp
)

target_link_libraries(test_protocol
    PRIVATE
        GTest::gtest_main
)

gtest_discover_tests(test_protocol)

add_executable(test_expiry
    test_expiry.cpp
)

target_link_libraries(test_expiry
    PRIVATE
        GTest::gtest_main
)

gtest_discover_tests(test_expiry)

add_executable(test_server
    test_server.cpp
)

target_link_libraries(test_server
    PRIVATE
        GTest::gtest_main
        $<$<PLATFORM_ID:Windows>:ws2_32>
)

gtest_discover_tests(test_server)

add_executable(test_persistence
    test_persistence.cpp
)

target_link_libraries(test_persistence
    PRIVATE
        GTest::gtest_main
)

gtest_discover_tests(test_persistence)
