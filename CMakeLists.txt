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

# WX4TAZ (AMD A6-3650 "Llano"): dynarmic's bundled xbyak 7.05 divides by zero
# in Cpu::setCacheHierarchy() on every AMD CPU without CPUID leaf 0xB (all AMD
# before Zen), killing the helper at qdv_init with code=0xc0000094. The patch
# backports upstream herumi/xbyak 53be4995fb3619cb59173450a64742265a4bc15f
# (xbyak 7.06) plus a divisor clamp. xbyak is header-only, so patching the
# fetched source at configure time covers every target built below. Applied
# once (a reverse check detects an already-patched tree); any other state
# stops the configure so an unpatched helper can never be built silently.
set(QDV_XBYAK_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/patches/xbyak_legacy_amd_div0.patch)
find_package(Git REQUIRED)
execute_process(
    COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${QDV_XBYAK_PATCH}
    WORKING_DIRECTORY ${dynarmic_SOURCE_DIR}
    RESULT_VARIABLE QDV_XBYAK_ALREADY OUTPUT_QUIET ERROR_QUIET)
if(NOT QDV_XBYAK_ALREADY EQUAL 0)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply ${QDV_XBYAK_PATCH}
        WORKING_DIRECTORY ${dynarmic_SOURCE_DIR}
        RESULT_VARIABLE QDV_XBYAK_APPLY)
    if(NOT QDV_XBYAK_APPLY EQUAL 0)
        message(FATAL_ERROR "xbyak legacy-AMD patch did not apply to ${dynarmic_SOURCE_DIR}")
    endif()
    message(STATUS "xbyak legacy-AMD divide-by-zero patch applied")
else()
    message(STATUS "xbyak legacy-AMD divide-by-zero patch already applied")
endif()

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

# ── Bundled helper EXECUTABLE (Android arm64-v8a / x86_64; Windows x64) ───────
# An executable that runs the SAME dynarmic codec in its OWN process, driven over
# the framed stdin/stdout IPC the app's VocoderHelperClient already speaks. It is
# NOT downloaded: it SHIPS INSIDE the package and is spawned as a subprocess. The
# GPL codec is linked in here, so it stays in its own binary and its own process,
# never in the app — which is what preserves the GPL separation AND satisfies the
# store rules (Play's Device-and-Network-Abuse on Android; the Microsoft Store's
# no-out-of-package-code rule on Windows).
#   - Android: libqso_dmr_vocoder_helper.so, a PIE, in jniLibs/<abi>/, execve'd
#     from nativeLibraryDir (the only exec-allowed location under Android W^X;
#     only lib*.so files there get exec permission).
#   - Windows: qso_dmr_vocoder_helper.exe, bundled next to the app .exe and in the
#     MSIX/NSIS package, Process.start'd.
#
# Same sources as the module (the qdv_* ABI over the vendored GPL md380 codec +
# dynarmic), plus the IPC front end in src/helper_main.cpp. helper_main.cpp is
# portable: the IPC loop + qdv_* calls are plain C, the crash instrumentation
# forks POSIX-signals (Android) vs SEH (Windows), and Windows adds _setmode binary
# stdio. Linux/macOS have no bundled-helper path today, so the target is scoped to
# the two shipping platforms.
# ── D-Star / P25 software codecs (the helper's separate "dv" mode) ────────────
# 2026-10-05. mbelib (ISC) + the OP25 AMBE encoder and MMDVMHost tables (GPL, via
# DroidStar's mbe/) + Pavel Yazev's IMBE coder (GPL v3+), vendored UNMODIFIED in
# third_party/dv_codecs/ except the changes listed in NOTICE.md (the D-Star
# loudness fix). Built as a static library linked into the SAME helper
# executable; the helper only runs it when started as `<helper> dv`, in its own
# process, and that path never calls qdv_* (src/dv_mode.cpp). Nothing in the
# MD-380 targets above changes.
enable_language(C)
set(QDV_DS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dv_codecs/DroidStar)
file(GLOB QDV_IMBE_SRC ${QDV_DS}/imbe_vocoder/*.cc)
add_library(qso_dv_codecs STATIC
    ${QDV_IMBE_SRC}
    ${QDV_DS}/mbe/ambe3600x2400.c
    ${QDV_DS}/mbe/ambe3600x2450.c
    ${QDV_DS}/mbe/ecc.c
    ${QDV_DS}/mbe/mbelib.c
    ${QDV_DS}/mbe/mbevocoder.cpp
    src/dv_codec_core.cpp
    src/dv_mode.cpp
)
target_include_directories(qso_dv_codecs
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${QDV_DS} ${QDV_DS}/mbe ${QDV_DS}/imbe_vocoder
)
# The upstream code is C++11-era; build it as C++17 (as the bench did), not 20.
set_target_properties(qso_dv_codecs PROPERTIES CXX_STANDARD 17)
if(MSVC)
    target_compile_definitions(qso_dv_codecs PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
    target_compile_options(qso_dv_codecs PRIVATE /W1 /wd4244 /wd4305 /wd4838 /wd4996)
else()
    target_compile_options(qso_dv_codecs PRIVATE -w)
    target_link_libraries(qso_dv_codecs PUBLIC m)
endif()

# Level-tracking + frame-format test for the dv codecs (host and Android CLI).
add_executable(dv_test test/dv_test.cpp)
target_link_libraries(dv_test PRIVATE qso_dv_codecs)
# dv_test sections 8 and 10 drive the vendored coder class directly.
target_include_directories(dv_test PRIVATE ${QDV_DS} ${QDV_DS}/mbe ${QDV_DS}/imbe_vocoder)
set_target_properties(dv_test PROPERTIES CXX_STANDARD 17)
if(MSVC)
    target_compile_definitions(dv_test PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS _ALLOW_KEYWORD_MACROS)
endif()

if(ANDROID OR WIN32)
    add_executable(qso_dmr_vocoder_helper
        src/helper_main.cpp
        third_party/md380_vocoder_dynarmic/md380_vocoder.cpp
        src/firmware_runtime.cpp
        src/md380_unwrap.cpp
        src/qdv_abi.cpp
    )
    target_include_directories(qso_dmr_vocoder_helper
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/md380_vocoder_dynarmic
    )
    target_compile_definitions(qso_dmr_vocoder_helper PRIVATE QDV_BUILDING=1)
    target_link_libraries(qso_dmr_vocoder_helper PRIVATE dynarmic qso_dv_codecs)
    if(ANDROID)
        # PIE .so named lib* so Android packages + extracts it with exec permission.
        set_target_properties(qso_dmr_vocoder_helper PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            PREFIX "lib"
            OUTPUT_NAME "qso_dmr_vocoder_helper"
            SUFFIX ".so"
        )
        target_link_options(qso_dmr_vocoder_helper PRIVATE -pie)
    else() # WIN32 -> qso_dmr_vocoder_helper.exe (default .exe suffix, no lib prefix)
        set_target_properties(qso_dmr_vocoder_helper PROPERTIES
            OUTPUT_NAME "qso_dmr_vocoder_helper"
        )
    endif()
endif()
