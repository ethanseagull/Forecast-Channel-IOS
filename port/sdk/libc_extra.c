// C library pieces the channel expects from MSL that the host C library lacks
// or builds differently: case-insensitive compares, and 16-bit wchar_t string
// functions (the channel is built with -fshort-wchar).
//
// Windows' C runtime already has all of these with 16-bit wchar_t and MSL's
// swprintf semantics (%s takes a wide string), so this file is for iOS only.
#ifndef _WIN32
#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

int stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
int strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }

size_t wcslen(const wchar_t* s) {
    const wchar_t* p = s;
    while (*p) {
        p++;
    }
    return (size_t)(p - s);
}

wchar_t* wcscpy(wchar_t* dst, const wchar_t* src) {
    wchar_t* d = dst;
    while ((*d++ = *src++) != 0) {
    }
    return dst;
}

wchar_t* wcsncpy(wchar_t* dst, const wchar_t* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = 0;
    }
    return dst;
}

wchar_t* wcscat(wchar_t* dst, const wchar_t* src) {
    wcscpy(dst + wcslen(dst), src);
    return dst;
}

int wcscmp(const wchar_t* a, const wchar_t* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)*a - (int)*b;
}

wchar_t* wcschr(const wchar_t* s, wchar_t c) {
    for (;; s++) {
        if (*s == c) {
            return (wchar_t*)s;
        }
        if (*s == 0) {
            return NULL;
        }
    }
}

// swprintf: formats through narrow vsnprintf. Format strings in the channel
// are ASCII; %s / %ls arguments are wide (MSL semantics for swprintf), and are
// copied through untouched, so non-ASCII text survives.
int vswprintf(wchar_t* out, size_t n, const wchar_t* fmt, va_list ap) {
    size_t o = 0;
    if (n == 0) {
        return -1;
    }
#define PUT(ch)                                                                \
    do {                                                                       \
        if (o + 1 < n) {                                                       \
            out[o] = (wchar_t)(ch);                                            \
        }                                                                      \
        o++;                                                                   \
    } while (0)
    while (*fmt) {
        if (*fmt != L'%') {
            PUT(*fmt++);
            continue;
        }
        // Collect one conversion spec into a narrow buffer.
        char spec[32];
        size_t k = 0;
        spec[k++] = '%';
        fmt++;
        while (*fmt && k < sizeof(spec) - 2 && strchr("diouxXeEfFgGcsp%", (char)*fmt) == NULL) {
            spec[k++] = (char)*fmt++;
        }
        if (*fmt == 0) {
            break;
        }
        char conv = (char)*fmt++;
        spec[k++] = conv;
        spec[k] = 0;
        if (conv == '%') {
            PUT(L'%');
        } else if (conv == 's') {
            const wchar_t* ws = va_arg(ap, const wchar_t*);
            if (ws == NULL) {
                ws = L"(null)";
            }
            while (*ws) {
                PUT(*ws++);
            }
        } else if (conv == 'c') {
            PUT(va_arg(ap, int));
        } else {
            char buf[64];
            int len;
            int longs = 0;
            for (size_t i = 1; i + 1 < k; i++) {
                longs += spec[i] == 'l';
            }
            if (strchr("eEfFgG", conv)) {
                len = snprintf(buf, sizeof(buf), spec, va_arg(ap, double));
            } else if (conv == 'p') {
                len = snprintf(buf, sizeof(buf), spec, va_arg(ap, void*));
            } else if (longs >= 2) {
                len = snprintf(buf, sizeof(buf), spec, va_arg(ap, long long));
            } else if (longs == 1) {
                len = snprintf(buf, sizeof(buf), spec, va_arg(ap, long));
            } else {
                len = snprintf(buf, sizeof(buf), spec, va_arg(ap, int));
            }
            for (int i = 0; i < len && i < (int)sizeof(buf) - 1; i++) {
                PUT(buf[i]);
            }
        }
    }
    out[o < n ? o : n - 1] = 0;
#undef PUT
    return o < n ? (int)o : -1;
}

int swprintf(wchar_t* out, size_t n, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vswprintf(out, n, fmt, ap);
    va_end(ap);
    return r;
}

#endif // !_WIN32
