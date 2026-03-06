cmake_minimum_required(VERSION 3.16)
project(hms-claude-mem VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependencies
find_package(CURL REQUIRED)
find_library(HIREDIS_LIB hiredis REQUIRED)
find_path(HIREDIS_INCLUDE hiredis/hiredis.h REQUIRED)

# Main binary
file(GLOB SOURCES "src/*.cpp")
add_executable(hms_claude_mem ${SOURCES})

target_include_directories(hms_claude_mem PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${HIREDIS_INCLUDE}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(hms_claude_mem PRIVATE
    ${HIREDIS_LIB}
    ${CURL_LIBRARIES}
    pthread
)

install(TARGETS hms_claude_mem DESTINATION bin)

# Tests
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()

    file(GLOB TEST_SOURCES "tests/unit/*.cpp")
    # All src except main.cpp
    file(GLOB LIB_SOURCES "src/*.cpp")
    list(FILTER LIB_SOURCES EXCLUDE REGEX "main\\.cpp$")

    add_executable(run_tests ${TEST_SOURCES} ${LIB_SOURCES})
    target_include_directories(run_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${HIREDIS_INCLUDE}
        ${CURL_INCLUDE_DIRS}
    )
    target_link_libraries(run_tests PRIVATE
        GTest::GTest
        GTest::Main
        gmock
        ${HIREDIS_LIB}
        ${CURL_LIBRARIES}
        pthread
    )
    add_test(NAME unit_tests COMMAND run_tests)
endif()

message(STATUS "hms-claude-mem v${PROJECT_VERSION}")
message(STATUS "  Redis client: hiredis")
message(STATUS "  HTTP client: libcurl")
message(STATUS "  Tests: ${BUILD_TESTS}")
