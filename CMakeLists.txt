cmake_minimum_required(VERSION 3.22.1)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" fetchkit_version LIMIT_COUNT 1)
if(NOT fetchkit_version MATCHES "^((0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*))(-(alpha|beta|rc)[.](0|[1-9][0-9]*))?$")
    message(FATAL_ERROR "VERSION must contain major.minor.patch with an optional -alpha.N, -beta.N or -rc.N suffix")
endif()
# CMake 的项目版本仅接受数字；运行时和发布产物必须保留预发布后缀，避免误标为正式版。
set(fetchkit_numeric_version "${CMAKE_MATCH_1}")
project(fetchkit VERSION ${fetchkit_numeric_version} LANGUAGES CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
include(GNUInstallDirs)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/third_party/picosha2/picosha2.h" fetchkit_picosha2_sha256)
if(NOT fetchkit_picosha2_sha256 STREQUAL "b13c180161ffac8d0adc81e033e493c409457c4d1258ab9781ac80579ba3bdd8")
    message(FATAL_ERROR "Vendored PicoSHA2 does not match third_party/picosha2.lock.json")
endif()
set(FETCHKIT_SANITIZERS "" CACHE STRING "Linux test sanitizers: address,undefined or thread (separately)")
if(FETCHKIT_SANITIZERS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT FETCHKIT_SANITIZERS MATCHES "^(address,undefined|address|undefined|thread)$")
        message(FATAL_ERROR "Use Linux address,undefined or a separate thread sanitizer build")
    endif()
    add_compile_options("-fsanitize=${FETCHKIT_SANITIZERS}" -fno-omit-frame-pointer)
    add_link_options("-fsanitize=${FETCHKIT_SANITIZERS}")
endif()

add_library(fetchkit_core STATIC
    src/core/version.cpp
    src/core/c_api.cpp
    src/core/diagnostics.cpp
    src/core/engine.cpp
    src/core/download_session.cpp
    src/core/subscription.cpp
    src/core/retry_policy.cpp
    src/storage/staging_file.cpp
    src/storage/record.cpp
    src/storage/resource_store.cpp
    src/storage/storage_policy.cpp
    src/storage/verifier.cpp
    src/storage/fault.cpp
)
add_library(fetchkit::core ALIAS fetchkit_core)

target_compile_features(fetchkit_core PUBLIC cxx_std_17)
target_include_directories(fetchkit_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_include_directories(fetchkit_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
find_package(Threads REQUIRED)
target_link_libraries(fetchkit_core PRIVATE Threads::Threads)
if(ANDROID)
    target_link_libraries(fetchkit_core PRIVATE z log)
else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(fetchkit_core PRIVATE ZLIB::ZLIB)
endif()
if(MSVC)
    target_compile_options(fetchkit_core PRIVATE /utf-8)
endif()
if(UNIX)
    target_compile_definitions(fetchkit_core PRIVATE _FILE_OFFSET_BITS=64)
endif()
target_compile_definitions(fetchkit_core PRIVATE
    FETCHKIT_VERSION="${fetchkit_version}"
)
set_target_properties(fetchkit_core PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME fetchkit
    EXPORT_NAME core
)

set(fetchkit_cli_default OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR AND NOT ANDROID)
    set(fetchkit_cli_default ON)
endif()
option(FETCHKIT_BUILD_CLI "Build the FetchKit command-line example" ${fetchkit_cli_default})
option(FETCHKIT_BUILD_SIZE_PROBE "Build isolated baseline/digest native size targets" OFF)
if(FETCHKIT_BUILD_SIZE_PROBE)
    foreach(mode IN ITEMS baseline digest engine)
        add_library(fetchkit_${mode}_measure SHARED tools/size/native_measure.cpp)
        target_include_directories(fetchkit_${mode}_measure PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(fetchkit_${mode}_measure PRIVATE fetchkit::core)
        if(NOT MSVC)
            target_compile_options(fetchkit_${mode}_measure PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
        endif()
        if(UNIX AND NOT APPLE)
            target_link_options(fetchkit_${mode}_measure PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/tools/size/measure.map")
            set_property(TARGET fetchkit_${mode}_measure APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/size/measure.map")
        endif()
        if(mode STREQUAL "digest")
            target_compile_definitions(fetchkit_${mode}_measure PRIVATE FETCHKIT_MEASURE_DIGEST=1)
        elseif(mode STREQUAL "engine")
            target_compile_definitions(fetchkit_${mode}_measure PRIVATE FETCHKIT_MEASURE_ENGINE=1)
        endif()
    endforeach()
endif()
option(FETCHKIT_BUILD_TESTS "Build host tests and register them with CTest" OFF)
option(FETCHKIT_ENABLE_FAULT_INJECTION "Enable test-only storage failures and process barriers" OFF)
if(FETCHKIT_ENABLE_FAULT_INJECTION)
    if(NOT FETCHKIT_BUILD_TESTS OR ANDROID)
        message(FATAL_ERROR "Fault injection is only permitted in host test builds")
    endif()
    target_compile_definitions(fetchkit_core PRIVATE FETCHKIT_ENABLE_FAULT_INJECTION=1)
endif()
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
    target_include_directories(fetchkit_transport_curl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(fetchkit_transport_curl PRIVATE CURL::libcurl)
    set_target_properties(fetchkit_transport_curl PROPERTIES
        CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON EXPORT_NAME transport_curl)
    target_compile_definitions(fetchkit_core PRIVATE FETCHKIT_WITH_CURL=1)
    target_link_libraries(fetchkit_core PRIVATE fetchkit_transport_curl)
endif()

if(FETCHKIT_BUILD_PROBES)
    if(NOT FETCHKIT_WITH_CURL)
        message(FATAL_ERROR "FETCHKIT_BUILD_PROBES requires FETCHKIT_WITH_CURL")
    endif()
    find_package(Threads REQUIRED)
    add_executable(fetchkit_transport_probe tests/contract/transport_probe.cpp)
    target_include_directories(fetchkit_transport_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(fetchkit_transport_probe PRIVATE fetchkit_transport_curl CURL::libcurl Threads::Threads)
endif()

option(FETCHKIT_INSTALL "Install the production library, CLI and CMake package" OFF)
if(FETCHKIT_INSTALL)
    if(FETCHKIT_ENABLE_FAULT_INJECTION OR FETCHKIT_SANITIZERS)
        message(FATAL_ERROR "An installable package must not contain fault injection or sanitizers")
    endif()
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    if(ANDROID)
        set(FETCHKIT_PACKAGE_ANDROID ON)
    else()
        set(FETCHKIT_PACKAGE_ANDROID OFF)
    endif()
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl.lock.json" fetchkit_package_curl_lock)
    string(JSON FETCHKIT_PINNED_CURL_VERSION GET "${fetchkit_package_curl_lock}" version)
    install(TARGETS fetchkit_core EXPORT fetchkitTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    if(FETCHKIT_WITH_CURL)
        install(TARGETS fetchkit_transport_curl EXPORT fetchkitTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
    if(FETCHKIT_BUILD_CLI)
        install(TARGETS fetchkit_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
    install(DIRECTORY include/fetchkit DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    configure_package_config_file(cmake/fetchkit-config.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/fetchkit-config.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/fetchkit")
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/fetchkit-config-version.cmake"
        VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
    install(EXPORT fetchkitTargets NAMESPACE fetchkit:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/fetchkit")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/fetchkit-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/fetchkit-config-version.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/fetchkit")
    install(FILES README.md VERSION DESTINATION ${CMAKE_INSTALL_DOCDIR})
    install(DIRECTORY third_party/licenses/ DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/fetchkit")
    install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/fetchkit")
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
    add_executable(fetchkit_diagnostics_test tests/unit/diagnostics_test.cpp)
    target_include_directories(fetchkit_diagnostics_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(fetchkit_diagnostics_test PRIVATE fetchkit::core)
    add_test(NAME diagnostics COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/diagnostics_test.py"
        --driver "$<TARGET_FILE:fetchkit_diagnostics_test>" --work-root "${CMAKE_CURRENT_BINARY_DIR}/diagnostics")
    set_tests_properties(diagnostics PROPERTIES LABELS "unit" TIMEOUT 120)
    add_executable(fetchkit_session_test tests/unit/download_session_test.cpp)
    target_include_directories(fetchkit_session_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(fetchkit_session_test PRIVATE fetchkit::core)
    add_test(NAME download_session COMMAND fetchkit_session_test "${CMAKE_CURRENT_BINARY_DIR}/session-store")
    set_tests_properties(download_session PROPERTIES LABELS "unit" TIMEOUT 30)
    add_executable(fetchkit_storage_test tests/unit/storage_test.cpp)
    target_include_directories(fetchkit_storage_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(fetchkit_storage_test PRIVATE fetchkit::core)
    add_test(NAME storage COMMAND fetchkit_storage_test "${CMAKE_CURRENT_BINARY_DIR}/storage-unit"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/record-v1.hex" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/record-v2.hex")
    set_tests_properties(storage PROPERTIES LABELS "unit" TIMEOUT 90)
    add_executable(fetchkit_scheduler_test tests/unit/scheduler_test.cpp)
    target_include_directories(fetchkit_scheduler_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(fetchkit_scheduler_test PRIVATE fetchkit::core)
    add_test(NAME scheduler_storage COMMAND fetchkit_scheduler_test "${CMAKE_CURRENT_BINARY_DIR}/scheduler-unit")
    set_tests_properties(scheduler_storage PROPERTIES LABELS "unit;scheduler" TIMEOUT 180)
    if(FETCHKIT_WITH_CURL AND FETCHKIT_BUILD_CLI)
        add_executable(fetchkit_download_driver tests/protocol/download_driver.cpp)
        target_link_libraries(fetchkit_download_driver PRIVATE fetchkit::core)
        target_include_directories(fetchkit_download_driver PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/examples/cli")
        add_test(NAME download_protocol COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol/protocol_test.py"
            --driver "$<TARGET_FILE:fetchkit_download_driver>" --cli "$<TARGET_FILE:fetchkit_cli>"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/protocol" --report "${CMAKE_CURRENT_BINARY_DIR}/reports/m1-protocol.json")
        set_tests_properties(download_protocol PROPERTIES LABELS "protocol" TIMEOUT 180)
        add_test(NAME m3_protocol COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol/m3_test.py" --cli "$<TARGET_FILE:fetchkit_cli>"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/m3-protocol" --report "${CMAKE_CURRENT_BINARY_DIR}/reports/m3-protocol.json")
        set_tests_properties(m3_protocol PROPERTIES LABELS "protocol;scheduler" TIMEOUT 240)
        if(FETCHKIT_ENABLE_FAULT_INJECTION)
            add_executable(fetchkit_recovery_driver tests/recovery/recovery_driver.cpp)
            target_include_directories(fetchkit_recovery_driver PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/examples/cli")
            target_link_libraries(fetchkit_recovery_driver PRIVATE fetchkit::core)
            add_test(NAME download_recovery COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/recovery/recovery_test.py"
                --driver "$<TARGET_FILE:fetchkit_recovery_driver>" --work-root "${CMAKE_CURRENT_BINARY_DIR}/recovery"
                --report "${CMAKE_CURRENT_BINARY_DIR}/reports/m2-recovery.json")
            set_tests_properties(download_recovery PROPERTIES LABELS "recovery" TIMEOUT 300)
            if(FETCHKIT_BUILD_LARGE_FILE_TESTS)
                add_test(NAME recovery_large_file COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/recovery/large_file_test.py"
                    --driver "$<TARGET_FILE:fetchkit_recovery_driver>" --work-root "${CMAKE_CURRENT_BINARY_DIR}/recovery-large"
                    --report "${CMAKE_CURRENT_BINARY_DIR}/reports/m2-large-file.json")
                set_tests_properties(recovery_large_file PROPERTIES LABELS "large_file" TIMEOUT 900)
            endif()
        endif()
        if(FETCHKIT_BUILD_LARGE_FILE_TESTS)
            add_test(NAME download_large_file COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol/protocol_test.py"
                --driver "$<TARGET_FILE:fetchkit_download_driver>" --cli "$<TARGET_FILE:fetchkit_cli>"
                --work-root "${CMAKE_CURRENT_BINARY_DIR}/large-file" --report "${CMAKE_CURRENT_BINARY_DIR}/reports/m1-large-file.json" --large-file)
            set_tests_properties(download_large_file PROPERTIES LABELS "large_file" TIMEOUT 900)
        endif()
    endif()
    if(FETCHKIT_BUILD_CLI)
        add_test(NAME core_version COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/version_test.py"
            --cli "$<TARGET_FILE:fetchkit_cli>" --version-file "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
        set_tests_properties(core_version PROPERTIES LABELS "unit" TIMEOUT 10)
    endif()
    if(FETCHKIT_BUILD_PROBES)
        add_test(NAME curl_contract COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/run_curl_probe.py"
            --probe "$<TARGET_FILE:fetchkit_transport_probe>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/reports/curl-contract.json")
        set_tests_properties(curl_contract PROPERTIES LABELS "contract" TIMEOUT 180)
    endif()
endif()
