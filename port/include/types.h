// Port replacement for the decomp's include/types.h.
// Same typedefs, but with fixed sizes on every target: CodeWarrior's `long`
// is 32 bits, which Windows (LLP64) keeps but iOS (LP64) does not, so u32/s32
// are built on int, not long.
#ifndef TYPES_H
#define TYPES_H

#ifdef __cplusplus
#include <cstdarg>
#include <cstddef>
#include <new>
#else
#include <stdarg.h>
#include <stddef.h>
#endif

#include <stdint.h>

#include <port_compat.h>
#include <decomp/decomp.h>
#include <decomp/lang.h>

#include <macros.h>

// Pointer rounding without truncating 64-bit addresses.
#undef ROUND_UP_PTR
#define ROUND_UP_PTR(x, align)                                                 \
    ((void*)((((uintptr_t)(x)) + (align) - 1) & ~((uintptr_t)(align) - 1)))
#undef ROUND_DOWN_PTR
#define ROUND_DOWN_PTR(x, align) ((void*)(((uintptr_t)(x)) & ~((uintptr_t)(align) - 1)))

// Memory-mapped Wii hardware: on the port these are inert dummies so the SDK
// headers still parse. Nothing in the port may rely on their contents.
#undef DECL_HW_REGS
#define DECL_HW_REGS(NAME) NAME##_HW_REGS[256]
// Headers define these globals in every unit that includes them; weak
// definitions let the linker keep one copy.
#undef DECL_ADDRESS
#define DECL_ADDRESS(addr) PORT_ONE_COPY
#undef DECL_WEAK
#define DECL_WEAK __attribute__((weak))
#undef DECL_SECTION
#define DECL_SECTION(x)

typedef uint64_t u64;
typedef int64_t s64;

typedef uint32_t u32;
typedef int32_t s32;

typedef uint16_t u16;
typedef int16_t s16;

typedef uint8_t u8;
typedef int8_t s8;

typedef float f32;
typedef double f64;

typedef int UNKWORD;
typedef void UNKTYPE;

enum { FALSE, TRUE };
typedef int BOOL;

typedef void (*funcptr_t)(void);

#ifdef __cplusplus
static_assert(sizeof(u32) == 4 && sizeof(s32) == 4, "u32/s32 must be 32 bits");
#else
_Static_assert(sizeof(u32) == 4 && sizeof(s32) == 4, "u32/s32 must be 32 bits");
#endif

#endif
