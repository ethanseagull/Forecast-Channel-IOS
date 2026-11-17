// Texture block codecs for GPU-compressed uploads (port/gx/gx_texcodec.cpp).
//
// BC1 (DXT1) block: 8 bytes. color0, color1: little-endian RGB565; then 32
// bits (little-endian) of 2-bit indices, pixel (x, y) of the 4x4 block at
// bits 2 * (4 * y + x). color0 > color1: 4 colours (c0, c1, 2/3 c0 + 1/3 c1,
// 1/3 c0 + 2/3 c1), opaque. color0 <= color1: 3 colours (c0, c1, 1/2 c0 +
// 1/2 c1) and index 3 = transparent black. Blocks are stored row by row.
//
// ASTC 4x4 block: 16 bytes (LDR, any of the standard UNORM profiles).
#ifndef PORT_GX_TEXCODEC_H
#define PORT_GX_TEXCODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Transcodes `blocks` BC1 blocks into the same number of ASTC 4x4 blocks
// (block i -> block i), so a BC1 image of W x H blocks becomes an ASTC 4x4
// image of the same block grid. The ASTC result must decode to the BC1
// block's pixels as closely as ASTC's endpoint and weight quantisation allow
// (transparent pixels stay transparent, opaque ones opaque).
void gx_bc1_to_astc4x4(const uint8_t* bc1, size_t blocks, uint8_t* astc);

// Encodes an RGBA8 image (bytes R, G, B, A, rows `pitch` bytes apart) of
// w x h pixels into BC1, ceil(w / 4) x ceil(h / 4) blocks (edge blocks
// repeat the last row / column). Pixels with alpha < 128 become transparent
// (3-colour blocks); the rest is opaque.
void gx_bc1_encode(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch, uint8_t* bc1);

// Decodes BC1 blocks to RGBA8 (w x h, multiples of 4), for tests.
void gx_bc1_decode(const uint8_t* bc1, uint32_t w, uint32_t h, uint8_t* rgba);

#ifdef __cplusplus
}
#endif
#endif
