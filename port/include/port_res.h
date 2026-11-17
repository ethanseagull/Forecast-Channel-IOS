// Helpers for converting big-endian Wii resources to native byte order in
// place, and the per-format entry points (port/res/*.cpp).
#ifndef PORT_RES_H
#define PORT_RES_H

#include <string.h>

#ifdef __cplusplus
namespace port {
namespace res {

inline void Swap16(void* p) {
    unsigned short v;
    memcpy(&v, p, 2);
    v = __builtin_bswap16(v);
    memcpy(p, &v, 2);
}

inline void Swap32(void* p) {
    unsigned int v;
    memcpy(&v, p, 4);
    v = __builtin_bswap32(v);
    memcpy(p, &v, 4);
}

inline void Swap16n(void* p, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) {
        Swap16(static_cast<char*>(p) + i * 2);
    }
}

inline void Swap32n(void* p, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) {
        Swap32(static_cast<char*>(p) + i * 4);
    }
}

// Floats are swapped as raw 32-bit words.
inline void SwapF32n(void* p, unsigned int n) { Swap32n(p, n); }

// Read a big-endian value without modifying memory.
inline unsigned int ReadBE32(const void* p) {
    unsigned int v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

inline unsigned short ReadBE16(const void* p) {
    unsigned short v;
    memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}

} // namespace res
} // namespace port

namespace nw4r {
namespace lyt {
namespace port {
// RLYT / RLAN: called by Layout::Build and Layout::CreateAnimTransform.
void SwapLayoutToNative(const void* file);
void SwapAnimationToNative(const void* file);
} // namespace port
} // namespace lyt
} // namespace nw4r

namespace nw4r {
namespace g3d {
namespace port {
// BRRES (MDL0, TEX0, PLT0): called by SimpleModel before it builds the ResFile.
void SwapResFileToNative(void* file);
} // namespace port
} // namespace g3d
} // namespace nw4r

namespace nw4r {
namespace ut {
namespace port {
// RFNT / RFNA: called by ResFont::SetResource and the ArchiveFont entry points.
void SwapFontToNative(const void* file);
} // namespace port
} // namespace ut
} // namespace nw4r
#endif

#endif
