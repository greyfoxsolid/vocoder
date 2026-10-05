@echo off
REM qso_dmr_vocoder - Android arm64 module build (non-obfuscated, module artifact).
setlocal

set "MOD=%~dp0"
set "VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "NDK=C:\Users\Owner\AppData\Local\Android\Sdk\ndk\27.0.12077973"

REM NDK clang is selected by the toolchain file (no vcvars). CMake + Ninja from
REM VS; git for dynarmic FetchContent.
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;C:\Program Files\Git\cmd;%PATH%"

if "%BOOST_ROOT%"=="" set "BOOST_ROOT=%MOD%..\..\spike\dmr_vocoder\boost_1_88_0"

echo === configure (arm64-v8a, android-24, c++_static) ===
cmake -G Ninja -S "%MOD%." -B "%MOD%build-android" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" ^
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,-z,max-page-size=16384" ^
  -DCMAKE_TOOLCHAIN_FILE="%NDK%\build\cmake\android.toolchain.cmake" ^
  -DANDROID_ABI=arm64-v8a ^
  -DANDROID_PLATFORM=android-24 ^
  -DANDROID_STL=c++_static ^
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=BOTH ^
  -DCMAKE_POLICY_DEFAULT_CMP0167=OLD ^
  -DBoost_NO_BOOST_CMAKE=ON ^
  -DBOOST_ROOT="%BOOST_ROOT%" ^
  -DBoost_INCLUDE_DIR="%BOOST_ROOT%" ^
  -DBoost_ADDITIONAL_VERSIONS=1.88 || exit /b 2

echo === build ===
cmake --build "%MOD%build-android" --config Release || exit /b 3

echo === ANDROID MODULE BUILD OK ===
