// In-place big-endian -> native conversion of NW4R fonts: RFNT (ResFont) and
// the RFNA archive font (ArchiveFont, sheets Huffman-compressed). Done once
// before nw4r::ut reads a font file; the byte-order mark tells whether a
// buffer is still big-endian. Glyph sheet images and compressed streams are
// left untouched (texture data and CX streams have their own byte order).
#include <nw4r/ut.h>

#include <port_log.h>
#include <port_res.h>

namespace {

using namespace nw4r::ut;
using ::port::res::Swap16;
using ::port::res::Swap16n;
using ::port::res::Swap32;
using ::port::res::Swap32n;

inline u8* At(void* base, u32 offset) { return static_cast<u8*>(base) + offset; }
inline u32 RoundUp4(u32 v) { return (v + 3) & ~3u; }

void SwapFINF(u8* body) {
    // FontInformation: fontType, linefeed (u8), alterCharIndex (u16),
    // defaultWidth (3 x u8), encoding (u8), pGlyph/pWidth/pMap (offsets), bytes.
    Swap16(body + 2);
    Swap32n(body + 8, 3);
}

void SwapTGLPHeader(u8* body) {
    // FontTextureGlyph: 4 x u8, sheetSize u32, 6 x u16, sheetImage offset.
    Swap32(body + 4);
    Swap16n(body + 8, 6);
    Swap32(body + 0x14);
}

void SwapCWDH(u8* body) {
    // FontWidth: indexBegin, indexEnd (u16), pNext (offset), CharWidths bytes.
    Swap16n(body, 2);
    Swap32(body + 4);
}

void SwapCMAP(u8* body) {
    // FontCodeMap: ccodeBegin, ccodeEnd, mappingMethod, reserved (u16),
    // pNext (offset), then u16 mapInfo whose length depends on the method.
    Swap16n(body, 4);
    Swap32(body + 8);
    const u16 begin = *reinterpret_cast<u16*>(body);
    const u16 end = *reinterpret_cast<u16*>(body + 2);
    const u16 method = *reinterpret_cast<u16*>(body + 4);
    u8* info = body + 0xC;
    switch (method) {
    case 0: // direct: first glyph index
        Swap16(info);
        break;
    case 1: // table: one index per code
        Swap16n(info, end - begin + 1u);
        break;
    case 2: { // scan: count, then (code, index) pairs
        Swap16(info);
        Swap16n(info + 2, *reinterpret_cast<u16*>(info) * 2u);
        break;
    }
    }
}

// GLGR (archive only): counts and name offsets, then u32 size tables and
// "used by set" bit arrays, then the set names (chars).
void SwapGLGR(BinaryFileHeader* file, FontGlyphGroupsBlock* block) {
    FontGlyphGroups& g = block->body;
    Swap32(&g.sheetSize);
    Swap16(&g.glyphsPerSheet);
    Swap16(&g.numSet);
    Swap16(&g.numSheet);
    Swap16(&g.numCWDH);
    Swap16(&g.numCMAP);
    Swap16n(g.nameOffsets, g.numSet);

    u32 namesStart = sizeof(BinaryFileHeader) + block->header.size;
    for (int i = 0; i < g.numSet; i++) {
        if (g.nameOffsets[i] < namesStart) {
            namesStart = g.nameOffsets[i];
        }
    }
    u32 tables = RoundUp4(sizeof(BinaryFileHeader) + offsetof(FontGlyphGroupsBlock, body) +
                          offsetof(FontGlyphGroups, nameOffsets) + g.numSet * sizeof(u16));
    for (u32 o = tables; o + 4 <= namesStart; o += 4) {
        Swap32(At(file, o));
    }
}

} // namespace

namespace nw4r {
namespace ut {
namespace port {

void SwapFontToNative(const void* pFile) {
    BinaryFileHeader* file = static_cast<BinaryFileHeader*>(const_cast<void*>(pFile));
    if (file->byteOrder != 0xFFFE) {
        return; // already native
    }
    Swap32(&file->signature);
    Swap16(&file->byteOrder);
    Swap16(&file->version);
    Swap32(&file->fileSize);
    Swap16(&file->headerSize);
    Swap16(&file->dataBlocks);
    const bool archive = file->signature == FOURCC('R', 'F', 'N', 'A');

    u16 numSheet = 0;
    u8* p = At(file, file->headerSize);
    for (int i = 0; i < file->dataBlocks; i++) {
        BinaryBlockHeader* bh = reinterpret_cast<BinaryBlockHeader*>(p);
        Swap32(&bh->kind);
        Swap32(&bh->size);
        u8* body = p + sizeof(BinaryBlockHeader);
        switch (bh->kind) {
        case FOURCC('G', 'L', 'G', 'R'):
            SwapGLGR(file, reinterpret_cast<FontGlyphGroupsBlock*>(p));
            numSheet = reinterpret_cast<FontGlyphGroupsBlock*>(p)->body.numSheet;
            break;
        case FOURCC('F', 'I', 'N', 'F'):
            SwapFINF(body);
            break;
        case FOURCC('T', 'G', 'L', 'P'): {
            SwapTGLPHeader(body);
            const u16 format = *reinterpret_cast<u16*>(body + 0xA);
            if (archive && (format & 0x8000)) {
                // Compressed sheets: [u32 compressed size][CX Huffman stream] ...
                u8* sheet = At(file, *reinterpret_cast<u32*>(body + 0x14));
                for (int s = 0; s < numSheet; s++) {
                    Swap32(sheet);
                    sheet += 4 + *reinterpret_cast<u32*>(sheet);
                }
            }
            break;
        }
        case FOURCC('C', 'W', 'D', 'H'):
            SwapCWDH(body);
            break;
        case FOURCC('C', 'M', 'A', 'P'):
            SwapCMAP(body);
            break;
        }
        p += bh->size;
    }
    port_log("font: %s, %u bytes, %d blocks, %d sheets\n", archive ? "RFNA" : "RFNT",
             static_cast<unsigned>(file->fileSize), file->dataBlocks, numSheet);
}

} // namespace port
} // namespace ut
} // namespace nw4r
