cmake_minimum_required(VERSION 3.16)
project(hms-claude-mem VERSION 1.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependencies
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG QUIET)

# hiredis: prefer CONFIG mode (vcpkg/brew), fall back to find_library (apt)
find_package(hiredis CONFIG QUIET)
if(NOT hiredis_FOUND)
    find_library(HIREDIS_LIB hiredis REQUIRED)
    find_path(HIREDIS_INCLUDE hiredis/hiredis.h REQUIRED)
endif()

# Main binary
file(GLOB SOURCES "src/*.cpp")
add_executable(hms_claude_mem ${SOURCES})

target_include_directories(hms_claude_mem PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CURL_INCLUDE_DIRS}
)

if(hiredis_FOUND)
    target_link_libraries(hms_claude_mem PRIVATE hiredis::hiredis)
else()
    target_include_directories(hms_claude_mem PRIVATE ${HIREDIS_INCLUDE})
    target_link_libraries(hms_claude_mem PRIVATE ${HIREDIS_LIB})
endif()

target_include_directories(hms_claude_mem PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
# One definition of the version: PROJECT_VERSION, not a literal in a .cpp.
target_compile_definitions(hms_claude_mem PRIVATE HMS_CLAUDE_MEM_VERSION="${PROJECT_VERSION}")
target_link_libraries(hms_claude_mem PRIVATE ${CURL_LIBRARIES})

if(nlohmann_json_FOUND)
    target_link_libraries(hms_claude_mem PRIVATE nlohmann_json::nlohmann_json)
endif()

if(NOT WIN32)
    target_link_libraries(hms_claude_mem PRIVATE pthread)
endif()

# ---------------------------------------------------------------------------
# In-process embeddings via llama.cpp (bundled nomic-embed-text GGUF).
# WITH_LOCAL_EMBED=OFF builds a lean Ollama/OpenAI-only binary (no llama link).
# ---------------------------------------------------------------------------
option(WITH_LOCAL_EMBED "Build in-process llama.cpp embeddings" ON)
set(HMS_LLAMA_TAG "b9789" CACHE STRING "Pinned llama.cpp release tag")
set(HMS_MODEL_FILE "nomic-embed-text-v1.5.Q8_0.gguf")
set(HMS_MODEL_URL  "https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/main/${HMS_MODEL_FILE}")
set(HMS_MODEL_SHA256 "3e24342164b3d94991ba9692fdc0dd08e3fd7362e0aacc396a9a5c54a544c3b7")

if(WITH_LOCAL_EMBED)
    include(FetchContent)
    set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)  # we only use public llama.h
    set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)  # don't pull a second curl/ssl stack
    set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)  # static-link llama+ggml into our binary
    if(APPLE)
        set(GGML_METAL      ON CACHE BOOL "" FORCE)
        set(GGML_ACCELERATE ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(llama_cpp
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        GIT_TAG        ${HMS_LLAMA_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(llama_cpp)

    target_link_libraries(hms_claude_mem PRIVATE llama)  # ggml propagates transitively
    target_compile_definitions(hms_claude_mem PRIVATE HMS_WITH_LOCAL_EMBED)

    # Bundle the GGUF: download once at configure time (SHA256-verified), then
    # install it and copy it next to the binary so <exe-dir>/models resolves.
    option(HMS_DOWNLOAD_MODEL "Download the bundled embedding model at configure time" ON)
    if(HMS_DOWNLOAD_MODEL AND NOT EXISTS "${CMAKE_BINARY_DIR}/models/${HMS_MODEL_FILE}")
        message(STATUS "Downloading embedding model ${HMS_MODEL_FILE} (~139 MB)...")
        file(DOWNLOAD "${HMS_MODEL_URL}" "${CMAKE_BINARY_DIR}/models/${HMS_MODEL_FILE}"
             EXPECTED_HASH SHA256=${HMS_MODEL_SHA256}
             SHOW_PROGRESS)
    endif()
    if(EXISTS "${CMAKE_BINARY_DIR}/models/${HMS_MODEL_FILE}")
        add_custom_command(TARGET hms_claude_mem POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:hms_claude_mem>/models
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_BINARY_DIR}/models/${HMS_MODEL_FILE}"
                    $<TARGET_FILE_DIR:hms_claude_mem>/models/${HMS_MODEL_FILE}
            COMMENT "Staging embedding model next to binary")
        install(FILES "${CMAKE_BINARY_DIR}/models/${HMS_MODEL_FILE}" DESTINATION bin/models)
    endif()
endif()

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
        ${CMAKE_SOURCE_DIR}/third_party
        ${CURL_INCLUDE_DIRS}
    )
    target_compile_definitions(run_tests PRIVATE HMS_CLAUDE_MEM_VERSION="${PROJECT_VERSION}")
    target_link_libraries(run_tests PRIVATE
        GTest::GTest
        GTest::Main
        # Imported target, not the bare name "gmock": the bare name makes the
        # linker search only its default paths, which miss Homebrew's lib dir.
        GTest::gmock
        ${CURL_LIBRARIES}
    )
    if(hiredis_FOUND)
        target_link_libraries(run_tests PRIVATE hiredis::hiredis)
    else()
        target_include_directories(run_tests PRIVATE ${HIREDIS_INCLUDE})
        target_link_libraries(run_tests PRIVATE ${HIREDIS_LIB})
    endif()
    if(nlohmann_json_FOUND)
        target_link_libraries(run_tests PRIVATE nlohmann_json::nlohmann_json)
    endif()
    if(NOT WIN32)
        target_link_libraries(run_tests PRIVATE pthread)
    endif()
    if(WITH_LOCAL_EMBED)
        target_link_libraries(run_tests PRIVATE llama)
        target_compile_definitions(run_tests PRIVATE HMS_WITH_LOCAL_EMBED)
    endif()
    add_test(NAME unit_tests COMMAND run_tests)
endif()

# ---------------------------------------------------------------------------
# Parity bench (SDD-001 §6 gate): cosine(local Q8, ollama) over the live corpus.
# Off by default; needs WITH_LOCAL_EMBED + a Redis/Ollama to point at.
# ---------------------------------------------------------------------------
option(BUILD_PARITY_BENCH "Build the local-vs-ollama embedding parity bench" OFF)
if(BUILD_PARITY_BENCH AND WITH_LOCAL_EMBED)
    file(GLOB BENCH_LIB_SOURCES "src/*.cpp")
    list(FILTER BENCH_LIB_SOURCES EXCLUDE REGEX "main\\.cpp$")
    add_executable(parity_bench tools/parity_bench.cpp ${BENCH_LIB_SOURCES})
    target_include_directories(parity_bench PRIVATE ${CMAKE_SOURCE_DIR}/src ${CURL_INCLUDE_DIRS})
    target_compile_definitions(parity_bench PRIVATE HMS_WITH_LOCAL_EMBED)
    target_link_libraries(parity_bench PRIVATE llama ${CURL_LIBRARIES})
    if(hiredis_FOUND)
        target_link_libraries(parity_bench PRIVATE hiredis::hiredis)
    else()
        target_include_directories(parity_bench PRIVATE ${HIREDIS_INCLUDE})
        target_link_libraries(parity_bench PRIVATE ${HIREDIS_LIB})
    endif()
    if(nlohmann_json_FOUND)
        target_link_libraries(parity_bench PRIVATE nlohmann_json::nlohmann_json)
    endif()
    if(NOT WIN32)
        target_link_libraries(parity_bench PRIVATE pthread)
    endif()
endif()

# ---------------------------------------------------------------------------
# Store-parity bench (SDD-002 §6 gate): EmbeddedStore search vs Redis VSIM.
# Off by default; needs a Redis to point at. No llama/curl needed.
# ---------------------------------------------------------------------------
option(BUILD_STORE_PARITY "Build the embedded-store vs Redis search parity bench" OFF)
if(BUILD_STORE_PARITY)
    add_executable(store_parity_bench
        tools/store_parity_bench.cpp
        src/embedded_store.cpp
        src/redis_client.cpp)
    target_include_directories(store_parity_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(hiredis_FOUND)
        target_link_libraries(store_parity_bench PRIVATE hiredis::hiredis)
    else()
        target_include_directories(store_parity_bench PRIVATE ${HIREDIS_INCLUDE})
        target_link_libraries(store_parity_bench PRIVATE ${HIREDIS_LIB})
    endif()
    if(NOT WIN32)
        target_link_libraries(store_parity_bench PRIVATE pthread)
    endif()
endif()

message(STATUS "hms-claude-mem v${PROJECT_VERSION}")
message(STATUS "  Redis client: hiredis")
message(STATUS "  HTTP client: libcurl")
message(STATUS "  Tests: ${BUILD_TESTS}")
