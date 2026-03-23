cmake_minimum_required(VERSION 3.20)

project(ukcp_server LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(UKCP_SERVER_BUILD_TESTS "Build ukcp_server tests" ON)
option(UKCP_SERVER_ENABLE_STATS "Enable ukcp_server stats counters" OFF)

set(UKCP_SERVER_SOURCES
    third_party/ikcp/ikcp.c
    src/protocol.cpp
    src/platform_socket_common.cpp
    src/server.cpp
    src/server_lifecycle.cpp
    src/server_receive.cpp
    src/server_send.cpp
    src/server_update.cpp
    src/session.cpp
)

if(WIN32)
    list(APPEND UKCP_SERVER_SOURCES
        src/platform_poller_windows.cpp
        src/platform_socket_windows.cpp
    )
else()
    list(APPEND UKCP_SERVER_SOURCES
        src/platform_poller_linux.cpp
        src/platform_socket_linux.cpp
    )
endif()

add_library(ukcp_server ${UKCP_SERVER_SOURCES})

target_include_directories(ukcp_server
    PUBLIC
        include
    PRIVATE
        src
        third_party/ikcp
)

target_compile_definitions(ukcp_server PRIVATE
    $<$<PLATFORM_ID:Windows>:NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS>
)
target_compile_definitions(ukcp_server PUBLIC
    $<$<BOOL:${UKCP_SERVER_ENABLE_STATS}>:UKCP_ENABLE_STATS=1>
)
target_link_libraries(ukcp_server
    PUBLIC
        $<$<PLATFORM_ID:Windows>:Ws2_32>
)

add_executable(ukcp_echo_server
    examples/echo/main.cpp
)
target_link_libraries(ukcp_echo_server PRIVATE ukcp_server)
target_include_directories(ukcp_echo_server PRIVATE include)

add_executable(ukcp_windows_perf
    bench/windows_perf/main.cpp
)
target_link_libraries(ukcp_windows_perf PRIVATE ukcp_server)
target_include_directories(ukcp_windows_perf PRIVATE include src third_party/ikcp)

add_executable(ukcp_perf_server
    bench/perf_server/main.cpp
)
target_link_libraries(ukcp_perf_server PRIVATE ukcp_server)
target_include_directories(ukcp_perf_server PRIVATE include src third_party/ikcp)

add_executable(ukcp_perf_client
    bench/perf_client/main.cpp
)
target_link_libraries(ukcp_perf_client PRIVATE ukcp_server)
target_include_directories(ukcp_perf_client PRIVATE include src third_party/ikcp)

if(UKCP_SERVER_BUILD_TESTS)
    enable_testing()
    add_executable(ukcp_server_tests
        tests/test_main.cpp
        tests/protocol_test.cpp
        tests/session_layout_test.cpp
        tests/server_test.cpp
        tests/echo_test.cpp
        tests/split_perf_test.cpp
        tests/interop_csharp_test.cpp
        tests/quiescence_test.cpp
    )
    target_link_libraries(ukcp_server_tests PRIVATE ukcp_server)
    target_include_directories(ukcp_server_tests PRIVATE include src third_party/ikcp)
    add_test(NAME ukcp_server_tests COMMAND ukcp_server_tests)
endif()
