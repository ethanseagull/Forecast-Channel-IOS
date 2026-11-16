@echo off
rem Builds the GX texture tests with the Visual Studio Build Tools' Clang.
rem   tools\tex_test\build.bat <output dir>
rem Then:
rem   <output dir>\tex_test.exe --self      decoder checks on synthetic data
rem   <output dir>\cache_test.exe           gx_tex.cpp's cache on stub state
rem   py -I tools\tex_test\extract_tpl.py build\windows\data\content <tpl dir>
rem   <output dir>\tex_test.exe [--checker] <png dir> <tpl dir>
rem Keep the output (and the extracted TPLs, which are Nintendo data) out of
rem the repository. Starts from a clean PATH: devkitPro's MSYS2 shadows cmd.
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
if not exist "%OUT%" mkdir "%OUT%"

rem tex_test: the decoders alone (gx_tex.cpp with GX_TEX_DECODE_ONLY).
"%LLVM%\clang-cl.exe" /nologo /O2 /std:c++17 /EHsc /W4 /DGX_TEX_DECODE_ONLY /D_CRT_SECURE_NO_WARNINGS ^
    -Wno-missing-field-initializers ^
    /I"%ROOT%\port\gx" ^
    "%ROOT%\tools\tex_test\tex_test.cpp" "%ROOT%\port\gx\gx_tex.cpp" ^
    /Fo"%OUT%\\" /Fe"%OUT%\tex_test.exe" || exit /b 1

rem cache_test: the whole of gx_tex.cpp against stub registers and memory,
rem with the project's headers and flags (CMakeLists.txt).
set "P=%ROOT%\port\include"
set "D=%ROOT%\forecast-decomp"
"%LLVM%\clang++.exe" -O1 -g -Wall -Wextra ^
    -DFORECAST_PORT -DNOMINMAX -DNONMATCHING -D_CRT_NONSTDC_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS ^
    -D_USE_MATH_DEFINES -I"%P%" -I"%D%\include" -I"%D%\include\channel" -I"%D%\build\HAFE\include" ^
    -I"%ROOT%\port\gx" -fshort-wchar -fno-strict-aliasing -fwrapv -include "%P%\port_compat.h" -fno-rtti ^
    -fno-exceptions -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast -Wno-unknown-pragmas -Wno-multichar ^
    -Wno-microsoft-cast -Wno-ignored-attributes -Wno-unknown-attributes ^
    "%ROOT%\tools\tex_test\cache_test.cpp" "%ROOT%\port\gx\gx_tex.cpp" ^
    -o "%OUT%\cache_test.exe" || exit /b 1
echo Built %OUT%\tex_test.exe and %OUT%\cache_test.exe
