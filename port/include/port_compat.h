// Port compatibility layer: maps CodeWarrior/PowerPC-isms used by the decomp
// sources onto Clang (Windows x64 and iOS arm64). Force-included before every
// decomp and port source.
#ifndef PORT_COMPAT_H
#define PORT_COMPAT_H
#ifndef __ASSEMBLER__

#include <stddef.h>

#ifndef NONMATCHING
#define NONMATCHING 1
#endif

// decomp/compat.h normally only defines these for language servers.
#define COMPAT_H
// Any PowerPC asm the port has not replaced with C must fail to build.
#ifdef __cplusplus
#define ASM(...) static_assert(false, "port: unported PowerPC asm")
#else
#define ASM(...) _Static_assert(0, "port: unported PowerPC asm")
#endif
#define ASM_VOLATILE(...) ASM()
#define DECLTYPE(x) __typeof__(x)
// Memory-mapped Wii globals are defined by headers in every unit that includes
// them; keep one copy (COFF: selectany, Mach-O: weak).
#ifdef _WIN32
#define PORT_ONE_COPY __attribute__((selectany))
#else
#define PORT_ONE_COPY __attribute__((weak))
#endif
#define DECL_ADDRESS(...) PORT_ONE_COPY
#define FLEXIBLE_ARRAY(NAME) NAME[]

// __declspec is a keyword on Windows (the system headers need it, and Clang
// ignores CodeWarrior's section/weak forms with a warning). Elsewhere it only
// appears in decomp code, so it goes away.
#ifndef _WIN32
#define __declspec(...)
#endif
#define __alloca __builtin_alloca
#define __memclr(var, size) __builtin_memset((var), 0, (size))

#ifdef __cplusplus
#define PORT_EXTERN_C extern "C"
#else
#define PORT_EXTERN_C
#endif

// PowerPC intrinsics -> portable equivalents.
#define __abs(x) __builtin_abs(x)
#define __labs(x) __builtin_labs(x)
#define __fabs(x) __builtin_fabs(x)
#define __fabsf(x) __builtin_fabsf(x)
#define __fnabs(x) (-__builtin_fabs(x))
#define __fnabsf(x) (-__builtin_fabsf(x))
#define __cntlzw(x) ((x) == 0 ? 32 : __builtin_clz((unsigned)(x)))
#define __fmadd(a, b, c) ((a) * (b) + (c))
#define __fmadds(a, b, c) ((float)((a) * (b) + (c)))
#define __fmsub(a, b, c) ((a) * (b) - (c))
#define __fmsubs(a, b, c) ((float)((a) * (b) - (c)))
#define __fnmadd(a, b, c) (-((a) * (b) + (c)))
#define __fnmadds(a, b, c) ((float)-((a) * (b) + (c)))
#define __fnmsub(a, b, c) (-((a) * (b) - (c)))
#define __fnmsubs(a, b, c) ((float)-((a) * (b) - (c)))
#define __fres(x) (1.0f / (x))
#define __frsqrte(x) (1.0 / __builtin_sqrt(x))
#define __fsel(a, b, c) ((a) >= 0 ? (b) : (c))
#define __divw(a, b) ((int)(a) / (int)(b))
#define __divwu(a, b) ((unsigned)(a) / (unsigned)(b))
#define __mulhw(a, b) ((int)(((long long)(int)(a) * (int)(b)) >> 32))
#define __mulhwu(a, b) ((unsigned)(((unsigned long long)(unsigned)(a) * (unsigned)(b)) >> 32))
// rlwimi / rlwinm: rotate left, then insert under (or AND with) the mask of
// bits mb..me, numbered from the most significant bit.
static inline unsigned __port_ppc_mask(int mb, int me) {
    unsigned a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (a & b) : (a | b);
}
static inline unsigned __port_rotl(unsigned v, int sh) {
    sh &= 31;
    return sh ? (v << sh | v >> (32 - sh)) : v;
}
static inline unsigned __rlwimi(unsigned ra, unsigned rs, int sh, int mb, int me) {
    unsigned m = __port_ppc_mask(mb, me);
    return (__port_rotl(rs, sh) & m) | (ra & ~m);
}
static inline unsigned __rlwinm(unsigned rs, int sh, int mb, int me) {
    return __port_rotl(rs, sh) & __port_ppc_mask(mb, me);
}
#define __lhbrx(p, o) __builtin_bswap16(*(unsigned short*)((char*)(p) + (o)))
#define __lwbrx(p, o) __builtin_bswap32(*(unsigned*)((char*)(p) + (o)))
#define __sthbrx(v, p, o) (*(unsigned short*)((char*)(p) + (o)) = __builtin_bswap16(v))
#define __stwbrx(v, p, o) (*(unsigned*)((char*)(p) + (o)) = __builtin_bswap32(v))
// Cache / sync instructions: no-ops (memory is coherent on the host).
#define __dcbf(...) ((void)0)
#define __dcbst(...) ((void)0)
#define __dcbt(...) ((void)0)
#define __dcbtst(...) ((void)0)
#define __dcbz(...) ((void)0)
#define __eieio() ((void)0)
#define __isync() ((void)0)
#define __sync() ((void)0)
#define __mffs() 0.0
#define __setflm(x) ((void)0)

// MSL <cmath> extras.
#ifndef M_SQRT3
#define M_SQRT3 1.73205f
#endif

// CodeWarrior's wchar_t is 16 bits; every target builds with -fshort-wchar
// (already the size on Windows). The wide-string functions the channel uses
// come from port/sdk/libc_extra.c on targets whose C library has 32-bit ones.
#ifdef __cplusplus
extern "C" {
#endif
int stricmp(const char* a, const char* b);
int strnicmp(const char* a, const char* b, size_t n);
#ifdef __cplusplus
}
#include <cstring>
#include <cstdint>
namespace std {
using ::stricmp;
using ::strnicmp;
}
#endif

#endif // __ASSEMBLER__
#endif
