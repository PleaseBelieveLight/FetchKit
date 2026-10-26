cmake_minimum_required(VERSION 3.22.1)

project(fetchkit VERSION 0.1.0 LANGUAGES CXX)

add_library(fetchkit_core STATIC
    src/core/version.cpp
)
add_library(fetchkit::core ALIAS fetchkit_core)

target_compile_features(fetchkit_core PUBLIC cxx_std_17)
target_include_directories(fetchkit_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
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
option(FETCHKIT_WITH_CURL "Enable the host libcurl transport probe" OFF)

if(FETCHKIT_BUILD_CLI)
    add_executable(fetchkit_cli examples/cli/main.cpp)
    target_link_libraries(fetchkit_cli PRIVATE fetchkit::core)
    set_target_properties(fetchkit_cli PROPERTIES CXX_EXTENSIONS OFF)
endif()

if(FETCHKIT_WITH_CURL)
    if(ANDROID)
        message(FATAL_ERROR "The M0 curl probe is host-only; Android uses a separate platform probe")
    endif()
    find_package(CURL 7.85 REQUIRED)
    add_library(fetchkit_transport_curl STATIC src/transport/curl_probe_transport.cpp)
    target_compile_features(fetchkit_transport_curl PUBLIC cxx_std_17)
    target_include_directories(fetchkit_transport_curl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(fetchkit_transport_curl PRIVATE CURL::libcurl)
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
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME fixture_server COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/server/server_test.py")
    set_tests_properties(fixture_server PROPERTIES LABELS "unit" TIMEOUT 30)
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
