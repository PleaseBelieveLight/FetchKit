cmake_minimum_required(VERSION 3.22.1)

project(fetchkit VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(fetchkit_core STATIC
    src/core/version.cpp
    src/core/c_api.cpp
    src/core/engine.cpp
    src/core/download_session.cpp
    src/storage/staging_file.cpp
)
add_library(fetchkit::core ALIAS fetchkit_core)

target_compile_features(fetchkit_core PUBLIC cxx_std_17)
target_include_directories(fetchkit_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_include_directories(fetchkit_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
find_package(Threads REQUIRED)
target_link_libraries(fetchkit_core PRIVATE Threads::Threads)
if(UNIX)
    target_compile_definitions(fetchkit_core PRIVATE _FILE_OFFSET_BITS=64)
endif()
target_compile_definitions(fetchkit_core PRIVATE
    FETCHKIT_VERSION="${PROJECT_VERSION}"
)
set_target_properties(fetchkit_core PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME fetchkit
)

set(fetchkit_cli_default OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR AND NOT ANDROID)
    set(fetchkit_cli_default ON)
endif()
option(FETCHKIT_BUILD_CLI "Build the FetchKit command-line example" ${fetchkit_cli_default})
option(FETCHKIT_BUILD_TESTS "Build host tests and register them with CTest" OFF)
option(FETCHKIT_BUILD_PROBES "Build M0 host transport probes" OFF)
option(FETCHKIT_BUILD_LARGE_FILE_TESTS "Register opt-in actual >4 GiB transfer tests" OFF)
option(FETCHKIT_WITH_CURL "Enable the host libcurl download backend" OFF)
option(FETCHKIT_USE_PINNED_CURL "Build the verified curl source instead of using a system package" OFF)
set(FETCHKIT_CURL_ARCHIVE "" CACHE FILEPATH "Optional local copy of the pinned curl archive (SHA256 verified)")
if(FETCHKIT_USE_PINNED_CURL AND NOT FETCHKIT_WITH_CURL)
    message(FATAL_ERROR "FETCHKIT_USE_PINNED_CURL requires FETCHKIT_WITH_CURL")
endif()

if(FETCHKIT_BUILD_CLI)
    add_executable(fetchkit_cli examples/cli/main.cpp)
    target_link_libraries(fetchkit_cli PRIVATE fetchkit::core)
    set_target_properties(fetchkit_cli PROPERTIES CXX_EXTENSIONS OFF)
endif()

if(FETCHKIT_WITH_CURL)
    if(ANDROID)
        message(FATAL_ERROR "The M0 curl probe is host-only; Android uses a separate platform probe")
    endif()
    if(FETCHKIT_USE_PINNED_CURL)
        enable_language(C)
        include(cmake/pinned_curl.cmake)
        fetchkit_configure_pinned_curl()
    else()
        find_package(CURL 7.85 REQUIRED)
    endif()
    add_library(fetchkit_transport_curl STATIC src/transport/curl_transport.cpp)
    target_compile_features(fetchkit_transport_curl PUBLIC cxx_std_17)
    target_include_directories(fetchkit_transport_curl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(fetchkit_transport_curl PRIVATE CURL::libcurl)
    set_target_properties(fetchkit_transport_curl PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_definitions(fetchkit_core PRIVATE FETCHKIT_WITH_CURL=1)
    target_link_libraries(fetchkit_core PRIVATE fetchkit_transport_curl)
endif()

if(FETCHKIT_BUILD_PROBES)
    if(NOT FETCHKIT_WITH_CURL)
        message(FATAL_ERROR "FETCHKIT_BUILD_PROBES requires FETCHKIT_WITH_CURL")
    endif()
    find_package(Threads REQUIRED)
    add_executable(fetchkit_transport_probe tests/contract/transport_probe.cpp)
    target_link_libraries(fetchkit_transport_probe PRIVATE fetchkit_transport_curl CURL::libcurl Threads::Threads)
endif()

if(FETCHKIT_BUILD_TESTS)
    if(ANDROID)
        message(FATAL_ERROR "Host tests must be configured in a separate build directory")
    endif()
    enable_testing()
    enable_language(C)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME fixture_server COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/server/server_test.py")
    set_tests_properties(fixture_server PROPERTIES LABELS "unit" TIMEOUT 30)
    add_executable(fetchkit_c_api_test tests/unit/c_api_test.c)
    target_link_libraries(fetchkit_c_api_test PRIVATE fetchkit::core)
    set_target_properties(fetchkit_c_api_test PROPERTIES LINKER_LANGUAGE CXX)
    if(FETCHKIT_WITH_CURL)
        target_compile_definitions(fetchkit_c_api_test PRIVATE FETCHKIT_TEST_CURL=1)
    endif()
    add_test(NAME c_api COMMAND fetchkit_c_api_test "${CMAKE_CURRENT_BINARY_DIR}/c-api-store")
    set_tests_properties(c_api PROPERTIES LABELS "unit" TIMEOUT 20)
    add_executable(fetchkit_session_test tests/unit/download_session_test.cpp)
    target_include_directories(fetchkit_session_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(fetchkit_session_test PRIVATE fetchkit::core)
    add_test(NAME download_session COMMAND fetchkit_session_test "${CMAKE_CURRENT_BINARY_DIR}/session-store")
    set_tests_properties(download_session PROPERTIES LABELS "unit" TIMEOUT 30)
    if(FETCHKIT_WITH_CURL AND FETCHKIT_BUILD_CLI)
        add_executable(fetchkit_download_driver tests/protocol/download_driver.cpp)
        target_link_libraries(fetchkit_download_driver PRIVATE fetchkit::core)
        target_include_directories(fetchkit_download_driver PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/examples/cli")
        add_test(NAME download_protocol COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol/protocol_test.py"
            --driver "$<TARGET_FILE:fetchkit_download_driver>" --cli "$<TARGET_FILE:fetchkit_cli>"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/protocol" --report "${CMAKE_CURRENT_BINARY_DIR}/reports/m1-protocol.json")
        set_tests_properties(download_protocol PROPERTIES LABELS "protocol" TIMEOUT 180)
        if(FETCHKIT_BUILD_LARGE_FILE_TESTS)
            add_test(NAME download_large_file COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol/protocol_test.py"
                --driver "$<TARGET_FILE:fetchkit_download_driver>" --cli "$<TARGET_FILE:fetchkit_cli>"
                --work-root "${CMAKE_CURRENT_BINARY_DIR}/large-file" --report "${CMAKE_CURRENT_BINARY_DIR}/reports/m1-large-file.json" --large-file)
            set_tests_properties(download_large_file PROPERTIES LABELS "large_file" TIMEOUT 900)
        endif()
    endif()
    if(FETCHKIT_BUILD_CLI)
        add_test(NAME core_version COMMAND fetchkit_cli --version)
        set_tests_properties(core_version PROPERTIES LABELS "unit" PASS_REGULAR_EXPRESSION "${PROJECT_VERSION}")
    endif()
    if(FETCHKIT_BUILD_PROBES)
        add_test(NAME curl_contract COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/run_curl_probe.py"
            --probe "$<TARGET_FILE:fetchkit_transport_probe>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/reports/curl-contract.json")
        set_tests_properties(curl_contract PROPERTIES LABELS "contract" TIMEOUT 180)
    endif()
endif()
