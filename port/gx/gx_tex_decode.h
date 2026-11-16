// Wii GX texture decoding (gx_tex.cpp), free of the rest of the GX layer so it
// can be built and tested on its own (tools/tex_test: gx_tex.cpp compiled with
// GX_TEX_DECODE_ONLY defined).
#ifndef PORT_GX_TEX_DECODE_H
#define PORT_GX_TEX_DECODE_H

#include <stdint.h>

// The same typedefs as types.h (repeating an identical typedef is allowed), so
// this header does not need the decomp's headers.
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

// GXTexFmt values as they appear in BP image0 bits 20-23 and in TPL files.
enum {
    GX_TEXFMT_I4 = 0,
    GX_TEXFMT_I8 = 1,
    GX_TEXFMT_IA4 = 2,
    GX_TEXFMT_IA8 = 3,
    GX_TEXFMT_RGB565 = 4,
    GX_TEXFMT_RGB5A3 = 5,
    GX_TEXFMT_RGBA8 = 6,
    GX_TEXFMT_CI4 = 8,
    GX_TEXFMT_CI8 = 9,
    GX_TEXFMT_CI14 = 10,
    GX_TEXFMT_CMPR = 14,
};

// GXTlutFmt (BP 0x98 bits 10-11, TPL palette headers).
enum {
    GX_TLUTFMT_IA8 = 0,
    GX_TLUTFMT_RGB565 = 1,
    GX_TLUTFMT_RGB5A3 = 2,
};

// Decodes a level-0 image of w x h texels in its Wii (tiled, big-endian)
// layout at src into outRgba: w * h * 4 bytes, R, G, B, A, top row first.
// I4/I8 become (I, I, I, I) and IA4/IA8 (I, I, I, A), as Dolphin does.
// For CI4/CI8/CI14, tlut holds the palette as the GPU's TMEM does
// (big-endian 16-bit entries in tlutFmt) and must have 16 / 256 / 16384
// entries; the other formats ignore tlut. src must hold
// gx_tex_data_size(fmt, w, h) bytes. False for an unknown format or a zero
// size (outRgba is left untouched).
bool gx_tex_decode(u32 fmt, const u8* src, u32 w, u32 h, const u8* tlut, u32 tlutFmt, u8* outRgba);

// Bytes of a level-0 image in memory (whole tiles), 0 for an unknown format.
u32 gx_tex_data_size(u32 fmt, u32 w, u32 h);

// Palette entries a CI format can index (16, 256, 16384), 0 for other formats.
u32 gx_tex_tlut_entries(u32 fmt);

#endif
