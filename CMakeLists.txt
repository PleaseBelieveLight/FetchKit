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

if(FETCHKIT_BUILD_CLI)
    add_executable(fetchkit_cli examples/cli/main.cpp)
    target_link_libraries(fetchkit_cli PRIVATE fetchkit::core)
    set_target_properties(fetchkit_cli PROPERTIES CXX_EXTENSIONS OFF)
endif()
