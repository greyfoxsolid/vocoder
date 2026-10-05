@echo off
REM Builds and runs xbyak_legacy_amd_test.cpp (simulated CPUID, see that file)
REM against an xbyak include dir. Default: the patched header dynarmic builds
REM with (build-windows\_deps\dynarmic-src\externals\xbyak). Pass another dir
REM holding xbyak\xbyak_util.h to test a different header (for example the
REM unpatched one, which must print CRASH code=0xc0000094 for the Llano profile).
setlocal
set "MOD=%~dp0.."
set "INC=%~1"
if "%INC%"=="" set "INC=%MOD%\build-windows\_deps\dynarmic-src\externals\xbyak"
set "OUT=%~2"
if "%OUT%"=="" set "OUT=%MOD%\build-windows"
set "VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /EHsc /std:c++20 /O2 /I "%INC%" "%MOD%\test\xbyak_legacy_amd_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\xbyak_legacy_amd_test.exe" >nul || exit /b 2
"%OUT%\xbyak_legacy_amd_test.exe"
exit /b %ERRORLEVEL%
