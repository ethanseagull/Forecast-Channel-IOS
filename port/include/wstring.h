// Port: MSL's <wstring.h>. Windows' C runtime has 16-bit wchar_t functions;
// elsewhere (-fshort-wchar) the implementations come from port/sdk/libc_extra.c.
#ifndef WSTRING_H
#define WSTRING_H
#ifdef _WIN32
#include <wchar.h>
#else
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
wchar_t* wcschr(const wchar_t*, wchar_t);
wchar_t* wcscpy(wchar_t* dst, const wchar_t* src);
wchar_t* wcsncpy(wchar_t* dst, const wchar_t* src, size_t n);
wchar_t* wcscat(wchar_t* dst, const wchar_t* src);
size_t wcslen(const wchar_t*);
int wcscmp(const wchar_t*, const wchar_t*);
int swprintf(wchar_t* s, size_t n, const wchar_t* format, ...);
#ifdef __cplusplus
}
#endif
#endif
#endif
