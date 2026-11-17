// A reference ASTC 2D LDR block decoder written from the Khronos Data Format
// Specification ("ASTC Compressed Texture Image Formats"), independent of
// port/gx/gx_texcodec.cpp, to check the BC1 -> ASTC transcoder.
//
// Supports: void-extent blocks (LDR), all 2D block modes and weight grids
// (with infill), one partition, single and dual plane, ISE with bit-only and
// trit ranges, weight ranges 0..1/2/3/7/... that are bits-only plus 0..2,
// endpoint ranges that are bits-only (as the transcoder emits), CEM 0, 4, 8, 12.
// Anything else decodes to the error colour and reports why.
#ifndef TEXCODEC_TEST_ASTC_REF_H
#define TEXCODEC_TEST_ASTC_REF_H

#include <stdint.h>

namespace astc_ref {

// What the block was, for statistics.
struct Info {
    bool voidExtent = false;
    bool dualPlane = false;
    int cem = -1;
    int gridW = 0, gridH = 0;
    int weightRange = 0; // number of levels
    int endpointRange = 0; // number of levels
    const char* error = nullptr; // non-null: decoded to the error colour
};

// Decodes one block of a bw x bh footprint into bw * bh RGBA8 texels (row by
// row). out8: UNORM8 as the top 8 bits of the 16-bit result (decode_unorm8).
// outRound (optional): the 16-bit result rounded to 8 bits, as a GPU that
// decodes to UNORM16/float and samples into 8 bits might.
Info decode_block(const uint8_t block[16], int bw, int bh, uint8_t* out8, uint8_t* outRound = nullptr);

} // namespace astc_ref

#endif
