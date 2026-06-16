@echo off
REM qso_dmr_vocoder - Windows x64 module build (non-obfuscated, module artifact).
setlocal

set "MOD=%~dp0"
set "VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

REM CMake + Ninja from VS; git for dynarmic FetchContent. No firmware-embed tools needed.
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;C:\Program Files\Git\cmd;%PATH%"

REM Boost headers (dynarmic prereq). Override BOOST_ROOT in the environment to
REM point elsewhere; default reuses the spike's header-only Boost.
if "%BOOST_ROOT%"=="" set "BOOST_ROOT=%MOD%..\..\spike\dmr_vocoder\boost_1_88_0"

echo === configure ===
cmake -G Ninja -S "%MOD%." -B "%MOD%build-windows" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_POLICY_DEFAULT_CMP0167=OLD ^
  -DBoost_NO_BOOST_CMAKE=ON ^
  -DBOOST_ROOT="%BOOST_ROOT%" ^
  -DBoost_INCLUDE_DIR="%BOOST_ROOT%" ^
  -DBoost_ADDITIONAL_VERSIONS=1.88 || exit /b 2

echo === build ===
cmake --build "%MOD%build-windows" --config Release || exit /b 3

echo === WINDOWS MODULE BUILD OK ===
