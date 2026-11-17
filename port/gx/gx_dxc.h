// The Windows SDK's DXC (dxcompiler.dll + dxil.dll), loaded at run time to
// compile the specialized TEV shaders (gx_spec.cpp) to DXIL. Windows only.
// No port headers: gx_dxc.cpp includes <windows.h>.
#ifndef GX_DXC_H
#define GX_DXC_H

#include <string>
#include <vector>

// Finds and loads the compiler: $FORECAST_DXC_DIR, the executable's folder,
// then the newest Windows 10/11 SDK under "Windows Kits\10\bin". Fills *where
// with the folder used. False if none loads.
bool gx_dxc_load(std::string* where);

// Compiles HLSL (ps_6_0, entry point main) to signed DXIL. On failure *err
// holds the messages. Call from one thread at a time.
bool gx_dxc_compile(const std::string& src, std::vector<unsigned char>* dxil, std::string* err);

#endif
