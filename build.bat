@echo off
rem ------------------------------------------------------------------------
rem libfif v3.5 / fif v2.5 - FIF2 fractal image format
rem
rem libfif and fif:  Oskar Behzadpour
rem FIF2 format:     Rezar Behzad
rem Web:             www.FractalImageCompression.com
rem Contact:         oskar@fractalimagecompression.com
rem
rem Copyright (c) 2026 Oskar Behzadpour
rem ------------------------------------------------------------------------

rem ---------------------------------------------------------------------------
rem  libfif - Windows build
rem
rem    build.bat [release|debug|asan] [native|avx2|avx|sse2|generic] [shared] [clean]
rem
rem  Defaults: release native.  Produces libfif_static.lib and fif.exe in this
rem  folder; with "shared" also libfif.dll, its import library libfif.lib and
rem  its debug symbols libfif.pdb.
rem
rem  Needs clang++ (ships with the "C++ Clang tools for Windows" component of
rem  Visual Studio / Build Tools).  PNG and JPEG support is compiled in when a
rem  vcpkg tree with libpng and libjpeg-turbo is found; without one the build
rem  still succeeds and the codec reads and writes PNM only.
rem
rem    vcpkg install libpng libjpeg-turbo --triplet x64-windows-static
rem
rem  Point VCPKG_ROOT at the vcpkg tree if it is not in %USERPROFILE% or C:\.
rem ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion
rem  Everything below uses paths relative to the repository, so run there
rem  whatever directory the caller invoked us from.
pushd "%~dp0"

set "CFG=release"
set "ARCHSEL=native"
set "DOCLEAN=0"
set "SHARED=0"

:parseargs
if "%~1"=="" goto parsed
if /i "%~1"=="release"  set "CFG=release"     & shift & goto parseargs
if /i "%~1"=="debug"    set "CFG=debug"       & shift & goto parseargs
if /i "%~1"=="asan"     set "CFG=asan"        & shift & goto parseargs
if /i "%~1"=="native"   set "ARCHSEL=native"  & shift & goto parseargs
if /i "%~1"=="avx2"     set "ARCHSEL=avx2"    & shift & goto parseargs
if /i "%~1"=="avx"      set "ARCHSEL=avx"     & shift & goto parseargs
if /i "%~1"=="sse2"     set "ARCHSEL=sse2"    & shift & goto parseargs
if /i "%~1"=="generic"  set "ARCHSEL=generic" & shift & goto parseargs
if /i "%~1"=="shared"   set "SHARED=1"        & shift & goto parseargs
if /i "%~1"=="clean"    set "DOCLEAN=1"       & shift & goto parseargs
echo build: unknown option "%~1"
exit /b 2
:parsed

if "%DOCLEAN%"=="1" (
    echo cleaning
    if exist obj rmdir /s /q obj
    if exist obj-shared rmdir /s /q obj-shared
    del /q libfif_static.lib fif.exe fif.pdb fif.ilk 2>nul
    del /q libfif.dll libfif.lib libfif.exp libfif.pdb 2>nul
    exit /b 0
)

if not defined CXX set "CXX=clang++"
where %CXX% >nul 2>nul || (
    echo build: %CXX% not found on PATH.
    echo        Install the "C++ Clang tools for Windows" VS component, or open
    echo        an "x64 Native Tools Command Prompt", or set CXX yourself.
    exit /b 1
)

rem ---- instruction set ------------------------------------------------------
rem  -march=native picks whatever this machine has.  The explicit levels exist
rem  so a binary can be built for an older target, and so the SSE2/AVX kernels
rem  can actually be exercised on a machine that would otherwise select AVX2.
if "%ARCHSEL%"=="native"  set "ARCH=-march=native"
if "%ARCHSEL%"=="avx2"    set "ARCH=-mavx2 -mfma"
if "%ARCHSEL%"=="avx"     set "ARCH=-mavx -mno-fma"
if "%ARCHSEL%"=="sse2"    set "ARCH=-msse2 -mno-avx"
if "%ARCHSEL%"=="generic" set "ARCH="

rem ---- configuration --------------------------------------------------------
if "%CFG%"=="release" set "OPT=-O3 -fno-math-errno -funroll-loops -DNDEBUG"
if "%CFG%"=="debug"   set "OPT=-O0 -g -gcodeview"
if "%CFG%"=="asan"    set "OPT=-O1 -g -gcodeview -fsanitize=address -fno-omit-frame-pointer" & set "ARCH="

set "WARN=-Wall -Wextra -Wno-unused-parameter"
set "CPPFLAGS=-Iinclude -Isrc -D_CRT_SECURE_NO_WARNINGS"

set "CXXFLAGS=-std=c++20 %OPT% %WARN% %ARCH%"

rem ---- optional libpng / libjpeg via vcpkg ----------------------------------
rem  The static triplet is deliberate.  clang++ hands link.exe an unconditional
rem  -defaultlib:libcmt, and -fms-runtime-lib=dll does not override it, so libs
rem  built against the DLL CRT ("-static-md") collide with the static CRT in
rem  our own objects and leave __imp_* unresolved.  Matching libcmt on both
rem  sides also means fif.exe ships with no runtime DLL dependency.
if not defined VCPKG_TRIPLET set "VCPKG_TRIPLET=x64-windows-static"
set "VCPKG_PREFIX="
if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\installed\%VCPKG_TRIPLET%\include\png.h" (
    set "VCPKG_PREFIX=%VCPKG_ROOT%\installed\%VCPKG_TRIPLET%"
)
if not defined VCPKG_PREFIX if exist "%USERPROFILE%\vcpkg\installed\%VCPKG_TRIPLET%\include\png.h" (
    set "VCPKG_PREFIX=%USERPROFILE%\vcpkg\installed\%VCPKG_TRIPLET%"
)
if not defined VCPKG_PREFIX if exist "C:\vcpkg\installed\%VCPKG_TRIPLET%\include\png.h" (
    set "VCPKG_PREFIX=C:\vcpkg\installed\%VCPKG_TRIPLET%"
)

set "LDLIBS="
if defined VCPKG_PREFIX (
    set "CPPFLAGS=!CPPFLAGS! -DFIF_HAVE_PNG -DFIF_HAVE_JPEG -DFIF_HAVE_ZLIB -I"!VCPKG_PREFIX!\include""
    set "LDLIBS=-L"!VCPKG_PREFIX!\lib" -llibpng16 -lzs -ljpeg"
    echo image i/o   png, jpeg, pnm, zlib   ^(!VCPKG_PREFIX!^)
) else (
    echo image i/o   pnm only - no vcpkg libpng/libjpeg-turbo found
)
echo config      %CFG% %ARCHSEL%

rem ---- compile --------------------------------------------------------------
if not exist obj mkdir obj

set "SRCS=src\pool.cpp src\encode.cpp src\serialize.cpp src\decode.cpp src\encode_api.cpp src\render.cpp src\image.cpp src\imageio.cpp src\metrics.cpp src\deblock.cpp src\symtable.cpp src\ctxtrain.cpp src\debug.cpp src\format.cpp src\exif.cpp"
set "OBJS="

for %%F in (%SRCS%) do (
    echo cc          %%F
    %CXX% %CXXFLAGS% %CPPFLAGS% -c "%%F" -o "obj\%%~nF.o" || exit /b 1
    set "OBJS=!OBJS! obj\%%~nF.o"
)

echo cc          tools\fif_cli.cpp
%CXX% %CXXFLAGS% %CPPFLAGS% -c tools\fif_cli.cpp -o obj\fif_cli.o || exit /b 1

rem ---- archive and link -----------------------------------------------------
rem  The static library is libfif_static.lib; the plain name libfif.lib belongs
rem  to the import library of libfif.dll (the zlib / CMake convention).
echo ar          libfif_static.lib
if exist libfif_static.lib del /q libfif_static.lib
llvm-ar rcs libfif_static.lib !OBJS! 2>nul || lib /nologo /out:libfif_static.lib !OBJS! >nul || exit /b 1

echo link        fif.exe
%CXX% %CXXFLAGS% -o fif.exe obj\fif_cli.o libfif_static.lib %LDLIBS% -lshell32 || exit /b 1

rem ---- shared library (optional) --------------------------------------------
rem  Compiled a second time with FIF_BUILD_SHARED, so that only these objects
rem  carry dllexport: fif.exe above stays free of exports.  The image libraries
rem  and the C runtime are linked into the DLL; the C API keeps allocation and
rem  release inside it (fif_free, fif_image_free), so any caller runtime works.
rem  goto rather than an if-block: an expanded path with ")" would end the block.
if not "%SHARED%"=="1" goto noshared
if not exist obj-shared mkdir obj-shared
set "SOBJS="
for %%F in (%SRCS%) do (
    echo cc shared   %%F
    %CXX% %CXXFLAGS% -g -gcodeview %CPPFLAGS% -DFIF_BUILD_SHARED -c "%%F" -o "obj-shared\%%~nF.o" || exit /b 1
    set "SOBJS=!SOBJS! obj-shared\%%~nF.o"
)
echo link        libfif.dll
%CXX% %CXXFLAGS% -g -shared -o libfif.dll !SOBJS! %LDLIBS% -Wl,/implib:libfif.lib -Wl,/debug -Wl,/pdb:libfif.pdb || exit /b 1
:noshared

echo.
rem  setlocal restores the caller directory on exit, so the pushd above needs
rem  no matching popd; name the exe explicitly since "." is not on PATH.
"%~dp0fif.exe" version
endlocal
