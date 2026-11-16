// 32-bit pointer slots for structures that overlay Wii file data.
//
// Wii resources (TPL textures, fonts, the channel's forecast files) are loaded
// into memory and then "relocated": 32-bit file offsets are overwritten with
// pointers in place. On a 64-bit host a pointer no longer fits, and a struct
// with pointer members no longer matches the file layout. Such members are
// declared PTR32(T) instead (decomp include/macros.h):
//   - on the Wii:          T*
//   - port, C++:           PortPtr32<T>, 4 bytes, holding a GX address
//                          (port_mem.h) and converting to and from T*
//   - port, C:             u32 holding the same GX address (convert with
//                          U32_TO_PTR / PTR_TO_U32)
// Before relocation the slot holds the file's raw offset; code that relocates
// reads it with PTR32_RAW and writes the pointer back with plain assignment.
#ifndef PORT_PTR32_H
#define PORT_PTR32_H

#include <stdint.h>

#include <port_mem.h>

#ifdef __cplusplus
// Headers may be first included inside extern "C" blocks.
extern "C++" {
template <typename T> struct PortPtr32 {
    uint32_t addr;

    T* get() const { return (T*)port_gx_ptr(addr); }
    operator T*() const { return get(); }
    T* operator->() const { return get(); }
    T& operator*() const { return *get(); }
    T& operator[](int i) const { return get()[i]; }
    PortPtr32& operator=(T* p) {
        addr = port_gx_addr(p);
        return *this;
    }
};
static_assert(sizeof(PortPtr32<char>) == 4, "PortPtr32 must stay 4 bytes");
}
#define PTR32(T) PortPtr32<T>
#define PTR32_RAW(field) ((field).addr)
#else
#define PTR32(T) uint32_t
#define PTR32_RAW(field) (field)
#endif

#endif
