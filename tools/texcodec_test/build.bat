@echo off
rem Builds the texture codec test with the Visual Studio Build Tools' Clang.
rem   tools\texcodec_test\build.bat <output dir>
rem Then:
rem   <output dir>\texcodec_test.exe <output dir> [image.png ...]
rem runs the BC1 -> ASTC round trip on synthetic blocks, then encodes each
rem image (PSNR, timings) and writes crops into <output dir>. Keep the output
rem out of the repository. Also compiles port\gx\gx_texcodec.cpp with the
rem project's flags (x64 and arm64) to catch warnings. Starts from a clean
rem PATH: devkitPro's MSYS2 shadows cmd.
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
    "%ROOT%\tools\texcodec_test\texcodec_test.cpp" "%ROOT%\tools\texcodec_test\astc_ref.cpp" ^
    "%ROOT%\port\gx\gx_texcodec.cpp" "%SDL%\lib\x64\SDL3.lib" ^
    -o "%OUT%\texcodec_test.exe" || exit /b 1
copy /y "%SDL%\lib\x64\SDL3.dll" "%OUT%\" >nul || exit /b 1

rem gx_texcodec.cpp alone with the project's flags (CMakeLists.txt), for x64
rem and, syntax only, arm64.
set "P=%ROOT%\port\include"
set "D=%ROOT%\forecast-decomp"
set FLAGS=-std=c++17 -Wall -Wextra -DFORECAST_PORT -DNOMINMAX -DNONMATCHING -D_CRT_NONSTDC_NO_WARNINGS ^
    -D_CRT_SECURE_NO_WARNINGS -D_USE_MATH_DEFINES -I"%P%" -I"%D%\include" -I"%D%\include\channel" ^
    -I"%D%\build\HAFE\include" -fshort-wchar -fno-strict-aliasing -fwrapv -include "%P%\port_compat.h" ^
    -fno-rtti -fno-exceptions -Wno-unknown-pragmas -Wno-multichar -Wno-microsoft-cast -Wno-ignored-attributes ^
    -Wno-unknown-attributes
"%LLVM%\clang++.exe" -O2 %FLAGS% -c "%ROOT%\port\gx\gx_texcodec.cpp" -o "%OUT%\gx_texcodec_x64.o" || exit /b 1
"%LLVM%\clang++.exe" --target=aarch64-pc-windows-msvc -fsyntax-only %FLAGS% "%ROOT%\port\gx\gx_texcodec.cpp" || exit /b 1
echo Built %OUT%\texcodec_test.exe
