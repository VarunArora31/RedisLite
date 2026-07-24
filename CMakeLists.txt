cmake_minimum_required(VERSION 3.20)
project(RedisLite CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDEs / clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Compiler warnings ─────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# ── Optional sanitizers (pass -DSANITIZE=thread|address to cmake) ─────────────
if(SANITIZE)
    message(STATUS "Sanitizer enabled: ${SANITIZE}")
    add_compile_options(-fsanitize=${SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${SANITIZE})
endif()

# ── Google Test (system-installed via MSYS2/ucrt64) ───────────────────────────
# Falls back to FetchContent if not found on the system.
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(STATUS "System GTest not found — fetching from GitHub")
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
else()
    message(STATUS "Using system GTest: ${GTest_VERSION}")
endif()

# ── Project-wide include path ─────────────────────────────────────────────────
# All source headers live in src/; anything that includes them uses this path.
include_directories(${CMAKE_SOURCE_DIR}/src)

# ── Subdirectories ────────────────────────────────────────────────────────────
add_subdirectory(tests)
add_subdirectory(client)
# add_subdirectory(src)         # uncomment when src gets .cpp files
# add_subdirectory(benchmarks)  # uncomment when benchmarks are added
