// In-place big-endian -> native conversion of NW4R layout (RLYT) and layout
// animation (RLAN) binaries, done once when the channel hands one to
// nw4r::lyt::Layout. The file header's byte-order mark tells whether a buffer
// is still big-endian, so converting twice is harmless.
//
// Rules: integers and floats are swapped; char arrays, u8 fields, GXColor /
// ut::Color byte quadruples and padding are left alone. Colors stored as u32
// (vtxCols, textCols) are swapped: the code reads them as u32 values.
#include <nw4r/lyt.h>

#include <port_log.h>
#include <port_res.h>

namespace nw4r {
namespace lyt {
namespace {

using ::port::res::Swap16;
using ::port::res::Swap32;
using ::port::res::Swap32n;
using ::port::res::SwapF32n;

inline u8* At(void* base, u32 offset) { return static_cast<u8*>(base) + offset; }

void SwapPaneCommon(res::Pane* p) {
    // name/userData are chars; flag/basePosition/alpha are bytes.
    SwapF32n(&p->translate, 3);
    SwapF32n(&p->rotate, 3);
    SwapF32n(&p->scale, 2);
    SwapF32n(&p->size, 2);
}

void SwapTexCoords(void* p, u32 num) {
    // detail::TexCoord = math::VEC2[4]
    SwapF32n(p, num * 8);
}

void SwapMaterial(res::Material* m) {
    // name: chars. tevCols: GXColorS10 (4 x s16). tevKCols: GXColor bytes.
    for (int i = 0; i < TEVCOLOR_MAX; i++) {
        Swap16(&m->tevCols[i].r);
        Swap16(&m->tevCols[i].g);
        Swap16(&m->tevCols[i].b);
        Swap16(&m->tevCols[i].a);
    }
    Swap32(&m->resNum.bits);
    const MaterialResourceNum& n = m->resNum;

    u32 offset = sizeof(res::Material);
    res::TexMap* texMap = reinterpret_cast<res::TexMap*>(At(m, offset));
    for (int i = 0; i < n.GetTexMapNum(); i++) {
        Swap16(&texMap[i].texIdx);
    }
    offset += n.GetTexMapNum() * sizeof(res::TexMap);

    SwapF32n(At(m, offset), n.GetTexSRTNum() * sizeof(TexSRT) / 4);
    offset += n.GetTexSRTNum() * sizeof(TexSRT);

    offset += n.GetTexCoordGenNum() * sizeof(TexCoordGen); // bytes
    if (n.GetChanCtrlNum() != 0) {
        offset += sizeof(ChanCtrl); // bytes
    }
    if (n.GetMatColNum() != 0) {
        offset += sizeof(ut::Color); // bytes
    }
    if (n.HasTevSwapTable()) {
        offset += GX_MAX_TEVSWAP * sizeof(TevSwapMode); // bytes
    }

    SwapF32n(At(m, offset), n.GetIndTexSRTNum() * sizeof(TexSRT) / 4);
    // IndirectStage, TevStage, AlphaCompare and BlendMode are all bytes.
}

void SwapMaterialList(res::MaterialList* list) {
    Swap16(&list->materialNum);
    u32* offsets = reinterpret_cast<u32*>(At(list, sizeof(res::MaterialList)));
    Swap32n(offsets, list->materialNum);
    for (int i = 0; i < list->materialNum; i++) {
        SwapMaterial(reinterpret_cast<res::Material*>(At(list, offsets[i])));
    }
}

void SwapNameList(void* block, u16* num, u32 headerSize, u32 entrySize) {
    // txl1 / fnl1: count, then entries starting with a u32 name offset that is
    // relative to the first entry.
    Swap16(num);
    u8* entries = At(block, headerSize);
    for (int i = 0; i < *num; i++) {
        Swap32(entries + i * entrySize);
    }
}

void SwapLayoutBlock(res::DataBlockHeader* block) {
    const u32 kind = detail::GetSignatureInt(block->kind);
    switch (kind) {
    case res::Layout::SIGNATURE: {
        res::Layout* p = reinterpret_cast<res::Layout*>(block);
        SwapF32n(&p->layoutSize, 2);
        break;
    }
    case FOURCC('t', 'x', 'l', '1'): {
        res::TextureList* p = reinterpret_cast<res::TextureList*>(block);
        SwapNameList(p, &p->texNum, sizeof(res::TextureList), sizeof(res::Texture));
        break;
    }
    case FOURCC('f', 'n', 'l', '1'): {
        res::FontList* p = reinterpret_cast<res::FontList*>(block);
        SwapNameList(p, &p->fontNum, sizeof(res::FontList), sizeof(res::Font));
        break;
    }
    case FOURCC('m', 'a', 't', '1'):
        SwapMaterialList(reinterpret_cast<res::MaterialList*>(block));
        break;
    case res::Pane::SIGNATURE:
    case res::Bounding::SIGNATURE:
        SwapPaneCommon(reinterpret_cast<res::Pane*>(block));
        break;
    case res::Picture::SIGNATURE: {
        res::Picture* p = reinterpret_cast<res::Picture*>(block);
        SwapPaneCommon(p);
        Swap32n(p->vtxCols, VERTEXCOLOR_MAX);
        Swap16(&p->materialIdx);
        SwapTexCoords(At(p, sizeof(res::Picture)), p->texCoordNum);
        break;
    }
    case res::TextBox::SIGNATURE: {
        res::TextBox* p = reinterpret_cast<res::TextBox*>(block);
        SwapPaneCommon(p);
        Swap16(&p->textBufBytes);
        Swap16(&p->textStrBytes);
        Swap16(&p->materialIdx);
        Swap16(&p->fontIdx);
        Swap32(&p->textStrOffset);
        Swap32n(p->textCols, TEXTCOLOR_MAX);
        SwapF32n(&p->fontSize, 2);
        SwapF32n(&p->charSpace, 1);
        SwapF32n(&p->lineSpace, 1);
        // UTF-16 text
        u16* text = reinterpret_cast<u16*>(At(p, p->textStrOffset));
        for (u32 i = 0; i < p->textStrBytes / 2u; i++) {
            Swap16(&text[i]);
        }
        break;
    }
    case res::Window::SIGNATURE: {
        res::Window* p = reinterpret_cast<res::Window*>(block);
        SwapPaneCommon(p);
        SwapF32n(&p->inflation, 4);
        Swap32(&p->contentOffset);
        Swap32(&p->frameOffsetTableOffset);
        res::WindowContent* c = reinterpret_cast<res::WindowContent*>(At(p, p->contentOffset));
        Swap32n(c->vtxCols, VERTEXCOLOR_MAX);
        Swap16(&c->materialIdx);
        SwapTexCoords(At(c, sizeof(res::WindowContent)), c->texCoordNum);
        u32* frames = reinterpret_cast<u32*>(At(p, p->frameOffsetTableOffset));
        Swap32n(frames, p->frameNum);
        for (int i = 0; i < p->frameNum; i++) {
            Swap16(&reinterpret_cast<res::WindowFrame*>(At(p, frames[i]))->materialIdx);
        }
        break;
    }
    case res::Group::SIGNATURE:
        Swap16(&reinterpret_cast<res::Group*>(block)->paneNum);
        break;
    default:
        // pas1/pae1/grs1/gre1 carry no data; anything else is unknown.
        break;
    }
}

void SwapAnimTarget(void* target) {
    // res::AnimationTarget: id, target, curveType (u8), keyNum u16, keysOffset u32
    u8* t = static_cast<u8*>(target);
    Swap16(t + 4);
    Swap32(t + 8);
    const u8 curveType = t[2];
    const u16 keyNum = *reinterpret_cast<u16*>(t + 4);
    u8* keys = t + *reinterpret_cast<u32*>(t + 8);
    if (curveType == 1) { // step: f32 frame, u16 value, u16 pad
        for (u32 i = 0; i < keyNum; i++) {
            SwapF32n(keys + i * 8, 1);
            Swap16(keys + i * 8 + 4);
        }
    } else if (curveType == 2) { // hermite: f32 frame, value, slope
        SwapF32n(keys, keyNum * 3u);
    }
}

void SwapAnimBlock(res::AnimationBlock* b) {
    Swap16(&b->frameSize);
    Swap16(&b->fileNum);
    Swap16(&b->animContNum);
    Swap32(&b->animContOffsetsOffset);
    Swap32n(At(b, sizeof(res::AnimationBlock)), b->fileNum); // file name table

    u32* contents = reinterpret_cast<u32*>(At(b, b->animContOffsetsOffset));
    Swap32n(contents, b->animContNum);
    for (int c = 0; c < b->animContNum; c++) {
        res::AnimationContent* content = reinterpret_cast<res::AnimationContent*>(At(b, contents[c]));
        u32* infos = reinterpret_cast<u32*>(At(content, sizeof(res::AnimationContent)));
        Swap32n(infos, content->num);
        for (int i = 0; i < content->num; i++) {
            res::AnimationInfo* info = reinterpret_cast<res::AnimationInfo*>(At(content, infos[i]));
            Swap32(&info->kind);
            u32* targets = reinterpret_cast<u32*>(At(info, sizeof(res::AnimationInfo)));
            Swap32n(targets, info->num);
            for (int t = 0; t < info->num; t++) {
                SwapAnimTarget(At(info, targets[t]));
            }
        }
    }
}

// Common file walk. Returns false if the buffer was already native.
template <typename BlockFn> bool SwapFile(void* file, BlockFn fn) {
    res::BinaryFileHeader* h = static_cast<res::BinaryFileHeader*>(file);
    if (h->byteOrder != 0xFFFE) {
        return false; // already native (or not a big-endian NW4R file)
    }
    Swap16(&h->byteOrder);
    Swap16(&h->version);
    Swap32(&h->fileSize);
    Swap16(&h->headerSize);
    Swap16(&h->dataBlocks);

    u8* block = At(file, h->headerSize);
    for (int i = 0; i < h->dataBlocks; i++) {
        res::DataBlockHeader* bh = reinterpret_cast<res::DataBlockHeader*>(block);
        Swap32(&bh->size);
        fn(bh);
        block += bh->size;
    }
    return true;
}

} // namespace

namespace port {

void SwapLayoutToNative(const void* file) {
    if (!SwapFile(const_cast<void*>(file), SwapLayoutBlock)) {
        return;
    }
    // One-line summary per layout, to check the conversion on real data.
    const res::BinaryFileHeader* h = static_cast<const res::BinaryFileHeader*>(file);
    const u8* block = static_cast<const u8*>(file) + h->headerSize;
    int panes = 0;
    const char* firstPane = "";
    char text[24] = "";
    u32 firstUnits = 0; // first two UTF-16 code units of that text
    for (int i = 0; i < h->dataBlocks; i++) {
        const res::DataBlockHeader* bh = reinterpret_cast<const res::DataBlockHeader*>(block);
        const u32 kind = detail::GetSignatureInt(bh->kind);
        if (kind == res::Pane::SIGNATURE || kind == res::Picture::SIGNATURE || kind == res::TextBox::SIGNATURE ||
            kind == res::Window::SIGNATURE || kind == res::Bounding::SIGNATURE) {
            const res::Pane* pane = reinterpret_cast<const res::Pane*>(bh);
            if (panes++ == 1) {
                firstPane = pane->name;
            }
            if (kind == res::TextBox::SIGNATURE && text[0] == 0) {
                const res::TextBox* tb = reinterpret_cast<const res::TextBox*>(bh);
                const u16* w = reinterpret_cast<const u16*>(block + tb->textStrOffset);
                u32 n = 0;
                for (; n < sizeof(text) - 1 && n < tb->textStrBytes / 2u && w[n]; n++) {
                    text[n] = (w[n] >= 0x20 && w[n] < 0x80) ? static_cast<char>(w[n]) : '?';
                }
                text[n] = 0;
                firstUnits = static_cast<u32>(w[0]) << 16 | w[1];
            }
        }
        block += bh->size;
    }
    port_log("lyt: %u bytes, %d blocks, %d panes, pane[1]=%.16s, text=\"%s\" (%08lx)\n",
             static_cast<unsigned>(h->fileSize), h->dataBlocks, panes, firstPane, text,
             static_cast<unsigned long>(firstUnits));
}

void SwapAnimationToNative(const void* file) {
    SwapFile(const_cast<void*>(file), [](res::DataBlockHeader* bh) {
        if (detail::GetSignatureInt(bh->kind) == FOURCC('p', 'a', 'i', '1')) {
            SwapAnimBlock(reinterpret_cast<res::AnimationBlock*>(bh));
        }
    });
}

} // namespace port
} // namespace lyt
} // namespace nw4r
