// The Windows SDK's DXC, loaded at run time (gx_dxc.h). Only used to compile
// the specialized TEV shaders on Windows, so they can be checked against the
// ubershader there; without the SDK the port keeps the ubershader.
#ifdef _WIN32

#include "gx_dxc.h"

// (Not WIN32_LEAN_AND_MEAN: dxcapi.h needs the COM headers.)
#include <windows.h>

#include <dxcapi.h>

#include <stdlib.h>
#include <string.h>

namespace {

DxcCreateInstanceProc sCreate;

bool file_exists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    std::string s(n > 0 ? (size_t)n - 1 : 0, '\0');
    if (n > 1) {
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, NULL, NULL);
    }
    return s;
}

// Loads dxil.dll (it signs the output; dxcompiler.dll finds it by name once
// it is loaded) and dxcompiler.dll from dir (ends in a backslash).
bool try_dir(const std::wstring& dir) {
    std::wstring dxil = dir + L"dxil.dll", dxc = dir + L"dxcompiler.dll";
    if (!file_exists(dxc) || !file_exists(dxil)) {
        return false;
    }
    HMODULE hDxil = LoadLibraryW(dxil.c_str());
    if (hDxil == NULL) {
        return false;
    }
    HMODULE hDxc = LoadLibraryW(dxc.c_str());
    if (hDxc == NULL) {
        FreeLibrary(hDxil);
        return false;
    }
    sCreate = (DxcCreateInstanceProc)(void*)GetProcAddress(hDxc, "DxcCreateInstance");
    if (sCreate == NULL) {
        FreeLibrary(hDxc);
        FreeLibrary(hDxil);
        return false;
    }
    return true;
}

// "10.0.26100.0" -> comparable number.
unsigned long long version_of(const wchar_t* name) {
    unsigned long long v = 0;
    int parts = 0;
    const wchar_t* p = name;
    while (*p != 0 && parts < 4) {
        wchar_t* end;
        unsigned long n = wcstoul(p, &end, 10);
        if (end == p) {
            return 0;
        }
        v = v * 100000 + n;
        parts++;
        p = *end == L'.' ? end + 1 : end;
        if (*end != L'.' && *end != 0) {
            return 0;
        }
    }
    return parts == 4 ? v : 0;
}

} // namespace

bool gx_dxc_load(std::string* where) {
    if (sCreate != NULL) {
        return true;
    }
    std::vector<std::wstring> dirs;
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"FORECAST_DXC_DIR", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring d(buf);
        if (d.back() != L'\\' && d.back() != L'/') {
            d += L'\\';
        }
        dirs.push_back(d);
    }
    n = GetModuleFileNameW(NULL, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring d(buf);
        size_t slash = d.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dirs.push_back(d.substr(0, slash + 1));
        }
    }
    // The newest SDK version under Windows Kits\10\bin.
    std::wstring kits;
    n = GetEnvironmentVariableW(L"ProgramFiles(x86)", buf, MAX_PATH);
    kits = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L"C:\\Program Files (x86)");
    kits += L"\\Windows Kits\\10\\bin\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((kits + L"10.*").c_str(), &fd);
    std::vector<std::pair<unsigned long long, std::wstring>> sdks;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                unsigned long long v = version_of(fd.cFileName);
                if (v != 0) {
                    sdks.push_back({v, kits + fd.cFileName + L"\\x64\\"});
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (size_t i = 0; i < sdks.size(); i++) {
        for (size_t j = i + 1; j < sdks.size(); j++) {
            if (sdks[j].first > sdks[i].first) {
                std::swap(sdks[i], sdks[j]);
            }
        }
    }
    for (auto& s : sdks) {
        dirs.push_back(s.second);
    }
    for (const std::wstring& d : dirs) {
        if (try_dir(d)) {
            *where = narrow(d);
            return true;
        }
    }
    return false;
}

bool gx_dxc_compile(const std::string& src, std::vector<unsigned char>* dxil, std::string* err) {
    // One compiler per thread (IDxcCompiler3 is not free-threaded).
    static thread_local IDxcCompiler3* tCompiler;
    if (sCreate == NULL) {
        *err = "dxcompiler.dll not loaded";
        return false;
    }
    if (tCompiler == NULL && FAILED(sCreate(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&tCompiler))) {
        tCompiler = NULL;
        *err = "DxcCreateInstance failed";
        return false;
    }
    DxcBuffer buf;
    buf.Ptr = src.data();
    buf.Size = src.size();
    buf.Encoding = DXC_CP_UTF8;
    // As tools/build_shaders.bat compiles the ubershader.
    LPCWSTR args[] = {L"-T", L"ps_6_0", L"-E", L"main", L"-O3", L"-Qstrip_debug", L"-Qstrip_reflect"};
    IDxcResult* res = NULL;
    if (FAILED(tCompiler->Compile(&buf, args, (UINT32)(sizeof(args) / sizeof(args[0])), NULL, __uuidof(IDxcResult),
                                  (void**)&res)) ||
        res == NULL) {
        *err = "Compile call failed";
        return false;
    }
    HRESULT status = E_FAIL;
    res->GetStatus(&status);
    bool ok = false;
    if (FAILED(status)) {
        IDxcBlobUtf8* msgs = NULL;
        if (SUCCEEDED(res->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), (void**)&msgs, NULL)) && msgs != NULL) {
            err->assign(msgs->GetStringPointer(), msgs->GetStringLength());
            msgs->Release();
        } else {
            *err = "compile failed (no messages)";
        }
    } else {
        IDxcBlob* obj = NULL;
        if (SUCCEEDED(res->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), (void**)&obj, NULL)) && obj != NULL) {
            const unsigned char* p = (const unsigned char*)obj->GetBufferPointer();
            size_t size = obj->GetBufferSize();
            // DXIL container: "DXBC", then a 16-byte digest that dxil.dll fills
            // in when it validates and signs. D3D12 rejects unsigned shaders.
            bool sign = size > 20 && memcmp(p, "DXBC", 4) == 0;
            for (int i = 4; sign && i < 20 && p[i] == 0; i++) {
                if (i == 19) {
                    sign = false;
                }
            }
            if (!sign) {
                *err = "output not signed (dxil.dll not used?)";
            } else {
                dxil->assign(p, p + size);
                ok = true;
            }
            obj->Release();
        } else {
            *err = "no object";
        }
    }
    res->Release();
    return ok;
}

#endif
