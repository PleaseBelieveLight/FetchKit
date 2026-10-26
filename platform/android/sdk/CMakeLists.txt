cmake_minimum_required(VERSION 3.22.1)

project(fetchkit_android LANGUAGES CXX)

add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    "${CMAKE_CURRENT_BINARY_DIR}/fetchkit-core"
)

add_library(fetchkit_jni SHARED src/main/cpp/fetchkit_jni.cpp)
option(FETCHKIT_ENABLE_PLATFORM_PROBE "Include the opt-in M0 Android/JNI probe" OFF)
if(FETCHKIT_ENABLE_PLATFORM_PROBE)
    target_sources(fetchkit_jni PRIVATE src/probe/cpp/platform_probe_jni.cpp)
    target_compile_definitions(fetchkit_jni PRIVATE FETCHKIT_ENABLE_PLATFORM_PROBE=1)
endif()
target_link_libraries(fetchkit_jni PRIVATE fetchkit::core)
target_compile_options(fetchkit_jni PRIVATE
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)
target_link_options(fetchkit_jni PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/fetchkit.map"
    -Wl,--no-undefined-version
)
set_target_properties(fetchkit_jni PROPERTIES CXX_EXTENSIONS OFF)
set_property(TARGET fetchkit_jni APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/fetchkit.map"
)
