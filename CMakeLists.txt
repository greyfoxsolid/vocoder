# qso_dmr_vocoder - standalone GPL DMR (AMBE+2) vocoder module.
#
# Builds a shared library exposing the stable extern "C" qdv_* ABI
# (include/qso_dmr_vocoder.h) over the vendored GPL md380 codec, with the
# firmware loaded from files at runtime (no firmware bytes in tree or artifact).
#
# Boost (headers only) is required by dynarmic; pass BOOST_ROOT at configure
# time. No python3/unzip/xxd/curl are needed (those were firmware-embed-time
# only in the spike and are gone with the runtime loader).

cmake_minimum_required(VERSION 3.20)
project(qso_dmr_vocoder CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

# Pinned to the exact commit the spike resolved to (was GIT_TAG master there).
set(DYNARMIC_TESTS OFF CACHE BOOL "")
set(DYNARMIC_USE_ARMV7 ON CACHE BOOL "")
FetchContent_Declare(
    dynarmic
    GIT_REPOSITORY https://github.com/yuzu-mirror/dynarmic.git
    GIT_TAG 9d4582339990d4eae53f1dc7160686920fc2075c
)
FetchContent_MakeAvailable(dynarmic)

add_library(qso_dmr_vocoder SHARED
    third_party/md380_vocoder_dynarmic/md380_vocoder.cpp
    src/firmware_runtime.cpp
    src/md380_unwrap.cpp
    src/qdv_abi.cpp
)
target_include_directories(qso_dmr_vocoder
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/md380_vocoder_dynarmic
)
# QDV_BUILDING -> dllexport on Windows. Only qdv_* (QDV_API / visibility
# default) are exported; everything else (md380_*, dynarmic) stays internal.
target_compile_definitions(qso_dmr_vocoder PRIVATE QDV_BUILDING=1)
target_link_libraries(qso_dmr_vocoder PRIVATE dynarmic)
set_target_properties(qso_dmr_vocoder PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Correctness + latency test runner, built for the same target platform.
add_executable(qdv_test test/qdv_test.cpp)
target_include_directories(qdv_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(qdv_test PRIVATE qso_dmr_vocoder)
