@echo off
rem Builds ForecastOS for Windows x64 with the Visual Studio Build Tools' Clang.
rem   tools\build_windows.bat            Debug build in build\windows
rem   tools\build_windows.bat Release    optimised build
rem Starts from a clean PATH: devkitPro's MSYS2 shadows cmd, cmake and ninja.
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
set "ROOT=%~dp0.."
set "PATH=%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\WindowsPowerShell\v1.0;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
for /f "usebackq delims=" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio Build Tools with the Clang component not found.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "LLVM=%VS%\VC\Tools\Llvm\x64\bin"
cmake -S "%ROOT%" -B "%ROOT%\build\windows" -G Ninja ^
    -DCMAKE_BUILD_TYPE=%CONFIG% ^
    -DCMAKE_C_COMPILER="%LLVM%\clang.exe" ^
    -DCMAKE_CXX_COMPILER="%LLVM%\clang++.exe" ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
    -DSDL3_DIR="%ROOT%\third_party\SDL3\cmake" || exit /b 1
cmake --build "%ROOT%\build\windows" -- -k 0
