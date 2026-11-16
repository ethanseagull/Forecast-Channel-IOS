// Port-side GX hooks for code outside port/gx.
#ifndef PORT_GX_H
#define PORT_GX_H
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Vertex arrays and display lists in [p, p + size) are in the Wii's byte
// order (resources the port keeps big-endian, such as model files). Arrays
// elsewhere are read natively.
void port_gx_register_be(const void* p, u32 size);
void port_gx_unregister_be(const void* p);

#ifdef __cplusplus
}
#endif
#endif
