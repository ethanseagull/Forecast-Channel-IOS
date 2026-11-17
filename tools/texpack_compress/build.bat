@echo off
rem Builds texpack_compress with the Visual Studio Build Tools' Clang.
rem   tools\texpack_compress\build.bat <output dir>
rem Then:
rem   <output dir>\texpack_compress.exe <pack dir> <output pack dir>
rem tools\pack_data.ps1 runs both. Starts from a clean PATH: devkitPro's
rem MSYS2 shadows cmd.
setlocal
if "%~1"=="" (
    echo usage: build.bat ^<output dir^>
    exit /b 2
)
set "OUT=%~f1"
set "ROOT=%~dp0..\.."
set "PATH=%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\WindowsPowerShell\v1.0;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
for /f "usebackq delims=" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio Build Tools with the Clang component not found.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "LLVM=%VS%\VC\Tools\Llvm\x64\bin"
set "SDL=%ROOT%\third_party\SDL3"
if not exist "%OUT%" mkdir "%OUT%"

"%LLVM%\clang++.exe" -O2 -std=c++17 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS ^
    -I"%ROOT%\port\gx" -isystem "%SDL%\include" ^
    "%ROOT%\tools\texpack_compress\texpack_compress.cpp" "%ROOT%\port\gx\gx_texcodec.cpp" ^
    "%SDL%\lib\x64\SDL3.lib" -o "%OUT%\texpack_compress.exe" || exit /b 1
copy /y "%SDL%\lib\x64\SDL3.dll" "%OUT%\" >nul || exit /b 1
echo Built %OUT%\texpack_compress.exe
