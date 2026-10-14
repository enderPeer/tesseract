@echo off
rem Builds alpha with MSVC.
rem   build         -> bin\alpha.exe
rem   build test    -> also builds and runs the tests
setlocal
cd /d "%~dp0"

where cl >nul 2>nul && goto :have_cl
rem vcvars64.bat itself also calls vswhere, so put it on the PATH
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo error: Visual Studio C++ build tools not found
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
:have_cl

if not exist bin mkdir bin
if not exist build mkdir build

set CFLAGS=/nologo /std:c11 /O2 /W4 /D_CRT_SECURE_NO_WARNINGS /Fo:build\
rem Only the lane engine is compiled for AVX2, so everything else still runs on any x64 CPU
rem and can check for AVX2 before using it.
cl %CFLAGS% /arch:AVX2 /c src\lanes.c || exit /b 1
set CORE=src\asm.c src\ref.c src\check.c src\platform.c src\x86.c src\jit.c src\parallel.c src\tester.c src\build.c src\net.c src\gen.c src\spec.c src\enum.c src\work.c src\benes.c src\cubedemo.c src\beta.c src\betacmd.c src\super.c src\supercmd.c src\gamma.c src\gammacmd.c build\lanes.obj

cl %CFLAGS% src\main.c %CORE% /Fe:bin\alpha.exe || exit /b 1

if /i not "%~1"=="test" exit /b 0
cl %CFLAGS% /Isrc tests\test_ref.c %CORE% /Fe:bin\test_ref.exe || exit /b 1
cl %CFLAGS% /Isrc tests\test_lanes.c %CORE% /Fe:bin\test_lanes.exe || exit /b 1
cl %CFLAGS% /Isrc tests\test_jit.c %CORE% /Fe:bin\test_jit.exe || exit /b 1
cl %CFLAGS% /Isrc tests\test_tester.c %CORE% /Fe:bin\test_tester.exe || exit /b 1
cl %CFLAGS% /Isrc tests\test_work.c %CORE% /Fe:bin\test_work.exe || exit /b 1
cl %CFLAGS% /Isrc tests\test_cube.c %CORE% /Fe:bin\test_cube.exe || exit /b 1
cl %CFLAGS% /Isrc tests\test_beta.c %CORE% /Fe:bin\test_beta.exe || exit /b 1
cl %CFLAGS% /Isrc tests\test_super.c %CORE% /Fe:bin\test_super.exe || exit /b 1
cl %CFLAGS% /Isrc tests\test_gamma.c %CORE% /Fe:bin\test_gamma.exe || exit /b 1
bin\test_ref.exe || exit /b 1
bin\test_lanes.exe || exit /b 1
bin\test_jit.exe || exit /b 1
bin\test_tester.exe || exit /b 1
bin\test_work.exe || exit /b 1
bin\test_cube.exe || exit /b 1
bin\test_beta.exe || exit /b 1
bin\test_super.exe || exit /b 1
bin\test_gamma.exe
