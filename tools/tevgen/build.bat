@echo off
rem Builds and runs the specialized TEV shader checks (port\gx\gx_tevgen.cpp)
rem with the Visual Studio Build Tools' Clang.
rem   tools\tevgen\build.bat <output dir> [random count]
rem 1. samples.exe writes the shaders for tools\tevgen\keys.txt plus random
rem    keys as HLSL and MSL into <output dir>\samples; every HLSL one is
rem    compiled with the Windows SDK's dxc (the MSL ones are compiled on the
rem    macOS CI runner: .github\workflows\ios.yml).
rem 2. gpu_test.exe draws each of those keys and the random registers with
rem    the ubershader and with the specialized shader on this PC's GPU (SDL3
rem    GPU, D3D12) and requires identical pixels.
rem Keep the output out of the repository. Starts from a clean PATH:
rem devkitPro's MSYS2 shadows cmd.
setlocal
if "%~1"=="" (
    echo usage: build.bat ^<output dir^> [random count]
    exit /b 2
)
set "OUT=%~f1"
set "COUNT=%~2"
if "%COUNT%"=="" set "COUNT=200"
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
set "DXC=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe"
if not exist "%OUT%\samples" mkdir "%OUT%\samples"

"%LLVM%\clang++.exe" -O2 -std=c++14 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -I"%ROOT%\port\gx" ^
    "%ROOT%\tools\tevgen\samples.cpp" "%ROOT%\port\gx\gx_tevgen.cpp" -o "%OUT%\samples.exe" || exit /b 1
"%LLVM%\clang++.exe" -O2 -std=c++17 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -I"%ROOT%\port\gx" ^
    -isystem "%SDL%\include" "%ROOT%\tools\tevgen\gpu_test.cpp" "%ROOT%\port\gx\gx_tevgen.cpp" ^
    "%ROOT%\port\gx\gx_dxc.cpp" "%SDL%\lib\x64\SDL3.lib" -o "%OUT%\gpu_test.exe" || exit /b 1
copy /y "%SDL%\lib\x64\SDL3.dll" "%OUT%\" >nul || exit /b 1

del /q "%OUT%\samples\*.hlsl" "%OUT%\samples\*.metal" 2>nul
"%OUT%\samples.exe" "%ROOT%\tools\tevgen\keys.txt" "%OUT%\samples" %COUNT% || exit /b 1
set FAILED=0
for %%f in ("%OUT%\samples\*.hlsl") do (
    "%DXC%" -nologo -T ps_6_0 -E main -O3 -WX -Fo "%OUT%\samples\%%~nf.dxil" "%%f" >nul 2>"%OUT%\samples\%%~nf.err" || (
        echo dxc failed: %%f
        type "%OUT%\samples\%%~nf.err"
        set FAILED=1
    )
)
if "%FAILED%"=="1" exit /b 1
echo HLSL samples compile.

"%OUT%\gpu_test.exe" "%ROOT%\tools\tevgen\keys.txt" %COUNT% || exit /b 1
