@echo off
REM qso_dmr_vocoder - Android armeabi-v7a (32-bit ARM) helper build.
REM
REM The v7a helper is NOT the dynarmic build: it runs the MD-380 firmware codec
REM natively on the 32-bit ARM host (nostar/md380_vocoder lineage, sources in
REM v7a\). This script is the recipe that used to live only in a memory note
REM (reference_v7a_helper_build_recipe): PIE executable, static libc++, the 9
REM --defsym firmware/SRAM addresses, then llvm-strip. With the dv sources
REM removed from the link it reproduces the shipped +585/TX v7a helper byte for
REM byte (proved 2026-10-05). 2026-10-05 adds the "dv" mode (D-Star / P25,
REM src\dv_mode.cpp + third_party\dv_codecs\), linked in exactly as on the
REM other three targets. 32-bit is exempt from Play's 16 KB page rule, so no
REM max-page-size flag (unchanged from the shipped build).
REM
REM Output: build-android-v7a\libqso_dmr_vocoder_helper.so (stripped) and
REM build-android-v7a\helper_unstripped.so. Copy the stripped one to
REM android\app\src\main\jniLibs\armeabi-v7a\.
setlocal enabledelayedexpansion

set "MOD=%~dp0"
set "NDK=C:\Users\Owner\AppData\Local\Android\Sdk\ndk\27.0.12077973\toolchains\llvm\prebuilt\windows-x86_64\bin"
set "TGT=--target=armv7a-linux-androideabi24"
set "OUT=%MOD%build-android-v7a"
set "DS=%MOD%third_party\dv_codecs\DroidStar"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\dv" mkdir "%OUT%\dv"

echo === MD-380 core, unwrap, helper front end ===
REM core.o MUST be compiled as C (keeps md380_* unmangled).
"%NDK%\clang.exe" %TGT% -fPIC -O2 -I"%MOD%v7a" -c "%MOD%v7a\md380_vocoder.c" -o "%OUT%\core.o" || exit /b 2
"%NDK%\clang++.exe" %TGT% -fPIC -O2 -I"%MOD%v7a" -c "%MOD%v7a\md380_unwrap.cpp" -o "%OUT%\unwrap.o" || exit /b 2
"%NDK%\clang++.exe" %TGT% -fPIC -O2 -I"%MOD%v7a" -I"%MOD%src" -c "%MOD%v7a\vocoder_helper.cpp" -o "%OUT%\helper.o" || exit /b 2

echo === dv codecs (D-Star / P25) ===
set "DVFLAGS=%TGT% -fPIC -O2 -w -I"%DS%" -I"%DS%\mbe" -I"%DS%\imbe_vocoder" -I"%MOD%src""
set "DVOBJS="
for %%F in ("%DS%\imbe_vocoder\*.cc") do (
  "%NDK%\clang++.exe" !DVFLAGS! -std=c++17 -c "%%~fF" -o "%OUT%\dv\%%~nF.o" || exit /b 3
  set "DVOBJS=!DVOBJS! "%OUT%\dv\%%~nF.o""
)
for %%F in (ambe3600x2400 ambe3600x2450 ecc mbelib) do (
  "%NDK%\clang.exe" !DVFLAGS! -c "%DS%\mbe\%%F.c" -o "%OUT%\dv\mbe_%%F.o" || exit /b 3
  set "DVOBJS=!DVOBJS! "%OUT%\dv\mbe_%%F.o""
)
"%NDK%\clang++.exe" !DVFLAGS! -std=c++17 -c "%DS%\mbe\mbevocoder.cpp" -o "%OUT%\dv\mbevocoder.o" || exit /b 3
"%NDK%\clang++.exe" !DVFLAGS! -std=c++17 -c "%MOD%src\dv_codec_core.cpp" -o "%OUT%\dv\dv_codec_core.o" || exit /b 3
"%NDK%\clang++.exe" !DVFLAGS! -std=c++17 -c "%MOD%src\dv_mode.cpp" -o "%OUT%\dv\dv_mode.o" || exit /b 3
set "DVOBJS=!DVOBJS! "%OUT%\dv\mbevocoder.o" "%OUT%\dv\dv_codec_core.o" "%OUT%\dv\dv_mode.o""

echo === link (PIE, static libc++, 9 --defsym firmware/SRAM addresses) ===
"%NDK%\clang++.exe" %TGT% -fPIC -pie -static-libstdc++ ^
  -o "%OUT%\helper_unstripped.so" "%OUT%\helper.o" "%OUT%\core.o" "%OUT%\unwrap.o" !DVOBJS! ^
  -Wl,--defsym,ambe_unpack=0x08048c9d ^
  -Wl,--defsym,ambe_en_mystery=0x2000c730 ^
  -Wl,--defsym,wav_inbuffer0=0x2000de82 ^
  -Wl,--defsym,wav_inbuffer1=0x2000df22 ^
  -Wl,--defsym,ambe_outbuffer=0x2000dfc6 ^
  -Wl,--defsym,ambe_mystery=0x20011224 ^
  -Wl,--defsym,ambe_outbuffer0=0x20011aa8 ^
  -Wl,--defsym,ambe_outbuffer1=0x20011b48 ^
  -Wl,--defsym,ambe_inbuffer=0x20011c8e ^
  -lm || exit /b 4
"%NDK%\llvm-strip.exe" "%OUT%\helper_unstripped.so" -o "%OUT%\libqso_dmr_vocoder_helper.so" || exit /b 5

echo === ANDROID armeabi-v7a HELPER BUILD OK ===
