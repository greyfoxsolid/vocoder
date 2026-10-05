@echo off
REM qso_dmr_vocoder - Android x86_64 module build (16 KB page aligned).
REM Sibling of build_android.bat (which builds arm64-v8a). Only difference:
REM ANDROID_ABI=x86_64 and the build dir. The -Wl,-z,max-page-size=16384 pair
REM makes the helper PIE's LOAD segments 16 KB aligned; without them the NDK r27
REM linker emits 4 KB (0x1000) LOAD segments, which Google Play rejected on
REM 4.0.0+633 (base/lib/x86_64/libqso_dmr_vocoder_helper.so "does not support
REM 16 KB pages"; arm64-v8a already passed because build_android.bat carries the
REM flags). VERIFY after building with the NDK llvm-readelf -l: every LOAD line
REM must read Align 0x4000.
setlocal

set "MOD=%~dp0"
set "VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "NDK=C:\Users\Owner\AppData\Local\Android\Sdk\ndk\27.0.12077973"

REM NDK clang is selected by the toolchain file (no vcvars). CMake + Ninja from
REM VS; git for dynarmic FetchContent.
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;C:\Program Files\Git\cmd;%PATH%"

if "%BOOST_ROOT%"=="" set "BOOST_ROOT=%MOD%..\..\spike\dmr_vocoder\boost_1_88_0"

echo === configure (x86_64, android-24, c++_static, 16 KB pages) ===
cmake -G Ninja -S "%MOD%." -B "%MOD%build-android-x64" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" ^
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,-z,max-page-size=16384" ^
  -DCMAKE_TOOLCHAIN_FILE="%NDK%\build\cmake\android.toolchain.cmake" ^
  -DANDROID_ABI=x86_64 ^
  -DANDROID_PLATFORM=android-24 ^
  -DANDROID_STL=c++_static ^
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=BOTH ^
  -DCMAKE_POLICY_DEFAULT_CMP0167=OLD ^
  -DBoost_NO_BOOST_CMAKE=ON ^
  -DBOOST_ROOT="%BOOST_ROOT%" ^
  -DBoost_INCLUDE_DIR="%BOOST_ROOT%" ^
  -DBoost_ADDITIONAL_VERSIONS=1.88 || exit /b 2

echo === build ===
cmake --build "%MOD%build-android-x64" --config Release || exit /b 3

echo === ANDROID x86_64 MODULE BUILD OK ===
