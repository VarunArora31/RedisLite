add_executable(redis-lite-cli
    cli.cpp
)

target_include_directories(redis-lite-cli PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(redis-lite-cli
    PRIVATE
        $<$<PLATFORM_ID:Windows>:ws2_32>
)
