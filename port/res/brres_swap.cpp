// In-place big-endian -> native conversion of NW4R g3d resource files (BRRES),
// done once before nw4r::g3d reads one (SimpleModel, the globe). The file
// header's byte-order mark tells whether a buffer is still big-endian, so
// converting twice is harmless.
//
// Converted: the root and folder dictionaries, MDL0 (header, info and its
// matrix-to-node table, every dictionary, nodes, vertex array headers and
// data, materials with their texture/palette info, texture SRT and channel
// data, TEV headers, shape headers and the texture/palette link tables), TEX0
// and PLT0 headers, and the u32 length in front of every name the file refers
// to (g3d compares them with its own native ResNameData).
//
// Left big-endian on purpose:
//   - GX display lists inside materials, TEV and shapes. They go to the GX
//     command processor as they are, and g3d's own readers and writers of the
//     registers in them (ResRead_u32, ResWriteBPCmd, ...) work byte by byte.
//   - Texel and palette data: the texture decoder reads the Wii formats.
//   - The shape vertex-descriptor cache (only compared for equality) and the
//     GXTexObj / GXTlutObj slots in materials (written natively at bind time).
//
// Vertex array data is converted to native, component by component (the
// header says the component type), because indexed vertex arrays are read
// native by the port's GX layer unless registered as big-endian, and because
// CPU code reads it too (shape-animation blending in g3d_calcvtx.cpp).
//
// Animation folders (CHR0, CLR0, PAT0, SRT0, SHP0, SCN0, VIS0) are not
// converted: no file the channel loads has one. Their folder dictionaries are
// still swapped, so counts are right, and a warning is logged.
#include <nw4r/g3d/res/g3d_resfile.h>

#include <cstddef>
#include <cstring>
#include <unordered_set>

#include <port_log.h>
#include <port_res.h>

namespace nw4r {
namespace g3d {
namespace {

using ::port::res::Swap16;
using ::port::res::Swap16n;
using ::port::res::Swap32;
using ::port::res::Swap32n;
using ::port::res::SwapF32n;

// The file layouts the converter relies on (and g3d reads in place).
static_assert(sizeof(ResDicNodeData) == 0x10, "ResDicNodeData");
static_assert(offsetof(ResMdlData, info) == 0x40, "ResMdlData");
static_assert(sizeof(ResMdlInfoData) == 0x40, "ResMdlInfoData");
static_assert(sizeof(ResNodeData) == 0xD0, "ResNodeData");
static_assert(sizeof(ResVtxPosData) == 0x38, "ResVtxPosData");
static_assert(sizeof(ResVtxNrmData) == 0x20, "ResVtxNrmData");
static_assert(sizeof(ResVtxClrData) == 0x20, "ResVtxClrData");
static_assert(offsetof(ResVtxTexCoordData, min) == 0x20, "ResVtxTexCoordData");
static_assert(offsetof(ResMatData, texObjData) == 0x3C, "ResMatData");
static_assert(offsetof(ResMatData, tlutObjData) == 0x140, "ResMatData");
static_assert(offsetof(ResMatData, texSrtData) == 0x1A4, "ResMatData");
static_assert(offsetof(ResMatData, chan) == 0x3EC, "ResMatData");
static_assert(sizeof(ResTexPlttInfoData) == 0x34, "ResTexPlttInfoData");
static_assert(sizeof(ResTexSrtData) == 0x248, "ResTexSrtData");
static_assert(sizeof(ResChanData) == 0x28, "ResChanData");
static_assert(offsetof(ResTevData, dl) == 0x20, "ResTevData");
static_assert(offsetof(ResShpData, msu) == 0x64, "ResShpData");
static_assert(sizeof(ResTexData) == 0x38, "ResTexData");
static_assert(sizeof(ResPlttData) == 0x28, "ResPlttData");

inline u8* At(void* base, s32 offset) { return static_cast<u8*>(base) + offset; }

class Converter {
public:
    explicit Converter(u8* file, u32 size) : mFile(file), mEnd(file + size) {}

    void Run();

    int mModels = 0, mTextures = 0, mPalettes = 0;
    int mMaterials = 0, mShapes = 0, mVtxArrays = 0;
    bool mOk = true;

private:
    // Each structure (and name) is converted once, even when several
    // dictionaries or materials share it.
    bool First(const void* p) { return mDone.insert(p).second; }

    bool InFile(const void* p, u32 size) const {
        const u8* b = static_cast<const u8*>(p);
        return b >= mFile && b <= mEnd && size <= static_cast<size_t>(mEnd - b);
    }

    // A name offset points at the characters; the u32 length is in front.
    void SwapName(void* base, s32 ofs) {
        if (ofs == 0) {
            return;
        }
        u8* len = At(base, ofs) - 4;
        if (!InFile(len, 4)) {
            Bad("name", base);
            return;
        }
        if (First(len)) {
            Swap32(len);
        }
    }

    void Bad(const char* what, const void* p) {
        if (mOk) {
            port_log("g3d: brres %s at +%lx is out of range; file left partly converted\n", what,
                     static_cast<unsigned long>(static_cast<const u8*>(p) - mFile));
        }
        mOk = false;
    }

    ResDicData* SwapDic(void* p);
    void SwapMdl(u8* m);
    void SwapNode(u8* n);
    void SwapVtx(u8* v, int kind);
    void SwapMat(u8* m);
    void SwapTev(u8* t);
    void SwapShp(u8* s);
    void SwapTexPlttInfoOffset(u8* p);
    void SwapTex(u8* t);
    void SwapPltt(u8* p);

    u8* mFile;
    u8* mEnd;
    std::unordered_set<const void*> mDone;
};

// ResDic: size, count, then count + 1 nodes (node 0 is the tree root). Data
// and name offsets are relative to the dictionary.
ResDicData* Converter::SwapDic(void* p) {
    if (!InFile(p, 8)) {
        Bad("dictionary", p);
        return NULL;
    }
    ResDicData* d = static_cast<ResDicData*>(p);
    if (!First(d)) {
        return d;
    }
    Swap32(&d->size);
    Swap32(&d->numData);
    if (!InFile(d->data, (d->numData + 1) * sizeof(ResDicNodeData))) {
        Bad("dictionary nodes", p);
        d->numData = 0;
        return d;
    }
    for (u32 i = 0; i <= d->numData; i++) {
        ResDicNodeData& n = d->data[i];
        Swap16(&n.ref);
        Swap16(&n.flag);
        Swap16(&n.idxLeft);
        Swap16(&n.idxRight);
        Swap32(&n.ofsString);
        Swap32(&n.ofsData);
        if (i != 0) {
            SwapName(d, n.ofsString);
        }
    }
    return d;
}

void Converter::SwapNode(u8* p) {
    if (!InFile(p, sizeof(ResNodeData)) || !First(p)) {
        return;
    }
    // Every field is a 32-bit integer, enum or float.
    Swap32n(p, sizeof(ResNodeData) / 4);
    ResNodeData* n = reinterpret_cast<ResNodeData*>(p);
    SwapName(n, n->name);
}

// Vertex arrays: kind 0 position, 1 normal, 2 color, 3 texture coordinate.
// The four headers share their first 0x20 bytes.
void Converter::SwapVtx(u8* p, int kind) {
    if (!InFile(p, 0x20) || !First(p)) {
        return;
    }
    ResVtxPosData* h = reinterpret_cast<ResVtxPosData*>(p); // common part only
    Swap32n(p, 7); // size, toResMdlData, toArray, name, id, cmpcnt, tp
    Swap16(p + 0x1E); // count
    SwapName(p, h->name);
    if (kind == 0) {
        SwapF32n(&h->min, 6); // min, max
    } else if (kind == 3) {
        ResVtxTexCoordData* t = reinterpret_cast<ResVtxTexCoordData*>(p);
        SwapF32n(&t->min, 4);
    }

    const u32 tp = static_cast<u32>(h->tp);
    const u32 cnt = static_cast<u32>(h->cmpcnt);
    const u32 num = *reinterpret_cast<u16*>(p + 0x1E);
    u8* data = At(p, h->toVtxPosArray);
    u32 avail = h->size > static_cast<u32>(h->toVtxPosArray) ? h->size - h->toVtxPosArray : 0;
    if (!InFile(data, avail)) {
        Bad("vertex array", p);
        return;
    }
    mVtxArrays++;

    if (kind == 2) {
        // Colors: only the 16-bit formats (RGB565, RGBA4) have a byte order;
        // RGB8 / RGBX8 / RGBA6 / RGBA8 are byte sequences.
        if (tp == GX_RGB565 || tp == GX_RGBA4) {
            u32 n = num;
            if (n * 2 > avail) {
                n = avail / 2;
            }
            Swap16n(data, n);
        }
        return;
    }

    u32 comps;
    switch (kind) {
    case 0: comps = cnt == GX_POS_XY ? 2 : 3; break;
    case 1: comps = cnt == GX_NRM_XYZ ? 3 : 9; break; // NBT / NBT3: N, B, T
    default: comps = cnt == GX_TEX_S ? 1 : 2; break;
    }
    u32 compSize = tp <= GX_S8 ? 1 : tp <= GX_S16 ? 2 : 4;
    u32 n = num * comps;
    if (n * compSize > avail) {
        n = avail / compSize;
    }
    if (compSize == 2) {
        Swap16n(data, n);
    } else if (compSize == 4) {
        Swap32n(data, n);
    }
}

void Converter::SwapTev(u8* p) {
    if (!InFile(p, sizeof(ResTevData)) || !First(p)) {
        return;
    }
    // size, toResMdlData, id; then bytes and the BP display list.
    Swap32n(p, 3);
}

void Converter::SwapMat(u8* p) {
    if (!InFile(p, sizeof(ResMatData)) || !First(p)) {
        return;
    }
    ResMatData* m = reinterpret_cast<ResMatData*>(p);
    mMaterials++;
    Swap32n(&m->size, 5); // size, toResMdlData, name, id, flag
    Swap32(&m->genMode.cullMode);
    // misc: bytes.
    Swap32(&m->toResTevData);
    Swap32(&m->numResTexPlttInfo);
    Swap32(&m->toResTexPlttInfo);
    Swap32(&m->toResUserData);
    Swap32(&m->toResMatDLData);
    // The GXTexObj / GXTlutObj slots are filled natively by ResMat::Bind.
    Swap32(&m->texObjData.flagUsedTexMapID);
    Swap32(&m->tlutObjData.flagUsedTlutID);

    ResTexSrtData& srt = m->texSrtData;
    Swap32(&srt.flag);
    Swap32(&srt.texMtxMode);
    SwapF32n(srt.texSrt, sizeof(srt.texSrt) / 4);
    for (int i = 0; i < ResTexSrtData::NUM_OF_TEXTURE; i++) {
        // ref_camera, ref_light, map_mode, misc_flag: bytes.
        SwapF32n(&srt.effect[i].effectMtx, 12);
    }

    for (int i = 0; i < 2; i++) {
        Chan& c = m->chan.chan[i];
        Swap32(&c.flag);
        // matColor / ambColor: GXColor bytes.
        Swap32(&c.paramChanCtrlC);
        Swap32(&c.paramChanCtrlA);
    }

    SwapName(m, m->name);

    if (m->toResUserData != 0) {
        port_log("g3d: material %s has user data (left big-endian)\n",
                 m->name != 0 ? reinterpret_cast<const char*>(At(m, m->name)) : "?");
    }

    if (m->numResTexPlttInfo != 0) {
        u8* info = At(m, m->toResTexPlttInfo);
        if (!InFile(info, m->numResTexPlttInfo * sizeof(ResTexPlttInfoData))) {
            Bad("texture info", m);
        } else {
            for (u32 i = 0; i < m->numResTexPlttInfo; i++) {
                u8* t = info + i * sizeof(ResTexPlttInfoData);
                if (!First(t)) {
                    continue;
                }
                // nameTex, namePltt, the two bind slots (0 in the file),
                // mapID, tlutID, wrap s/t, min/mag filter, lod_bias, max_aniso;
                // then two bools and padding.
                Swap32n(t, 12);
                ResTexPlttInfoData* r = reinterpret_cast<ResTexPlttInfoData*>(t);
                SwapName(r, r->nameTex);
                SwapName(r, r->namePltt);
            }
        }
    }

    if (m->toResTevData != 0) {
        SwapTev(At(m, m->toResTevData));
    }
}

void Converter::SwapShp(u8* p) {
    if (!InFile(p, sizeof(ResShpData)) || !First(p)) {
        return;
    }
    ResShpData* s = reinterpret_cast<ResShpData*>(p);
    mShapes++;
    Swap32(&s->size);
    Swap32(&s->toResMdlData);
    Swap32(&s->curMtxIdx);
    // cache: bytes, only compared for equality.
    Swap32n(&s->tagPrePrimDL, 3);
    Swap32n(&s->tagPrimDL, 3);
    Swap32n(&s->vcdBitmap, 6); // vcdBitmap, flag, name, id, numVtx, numPolygon
    Swap16(&s->idVtxPosition);
    Swap16(&s->idVtxNormal);
    Swap16n(s->idVtxColor, 2);
    Swap16n(s->idVtxTexCoord, 8);
    Swap32(&s->toMtxSetUsed);
    SwapName(s, s->name);

    if (s->toMtxSetUsed != 0) {
        u8* msu = At(s, s->toMtxSetUsed);
        if (InFile(msu, 4) && First(msu)) {
            Swap32(msu);
            u32 num = *reinterpret_cast<u32*>(msu);
            if (InFile(msu + 4, num * 2)) {
                Swap16n(msu + 4, num);
            } else {
                Bad("shape matrix list", s);
            }
        }
    }
}

// Texture/palette name -> (material, texture info) offset pairs.
void Converter::SwapTexPlttInfoOffset(u8* p) {
    if (!InFile(p, 4) || !First(p)) {
        return;
    }
    Swap32(p);
    u32 num = *reinterpret_cast<u32*>(p);
    if (!InFile(p + 4, num * sizeof(OffsetPair))) {
        Bad("texture link table", p);
        return;
    }
    Swap32n(p + 4, num * 2);
}

void Converter::SwapMdl(u8* p) {
    if (!InFile(p, sizeof(ResMdlData)) || !First(p)) {
        return;
    }
    ResMdlData* m = reinterpret_cast<ResMdlData*>(p);
    mModels++;
    // header.size, revision, toResFileData, the 11 dictionary offsets, name.
    Swap32n(&m->header.size, 15);
    SwapName(m, m->name);

    ResMdlInfoData& info = m->info;
    Swap32n(&info.size, 8); // ... numViewMtx; then four bytes
    Swap32(&info.toMtxIDToNodeID);
    // MDL0 version 8 info blocks can end before the bounding volume (earth's
    // is 0x28 bytes, with the matrix-to-node table right after it).
    if (info.size >= sizeof(ResMdlInfoData)) {
        SwapF32n(&info.volume_min, 6);
    }
    if (info.toMtxIDToNodeID != 0) {
        u8* t = At(&info, info.toMtxIDToNodeID);
        if (InFile(t, 4)) {
            Swap32(t);
            u32 num = *reinterpret_cast<u32*>(t);
            if (InFile(t + 4, num * 4)) {
                Swap32n(t + 4, num);
            } else {
                Bad("matrix-to-node table", t);
            }
        }
    }

    struct DicKind {
        s32 ResMdlData::*ofs;
        int kind;
    };
    enum { BYTECODE, NODE, POS, NRM, CLR, TEXCOORD, MAT, TEV, SHP, TEXINFO };
    static const DicKind kDics[] = {
        {&ResMdlData::toResByteCodeDic, BYTECODE},
        {&ResMdlData::toResNodeDic, NODE},
        {&ResMdlData::toResVtxPosDic, POS},
        {&ResMdlData::toResVtxNrmDic, NRM},
        {&ResMdlData::toResVtxClrDic, CLR},
        {&ResMdlData::toResVtxTexCoordDic, TEXCOORD},
        {&ResMdlData::toResMatDic, MAT},
        {&ResMdlData::toResTevDic, TEV},
        {&ResMdlData::toResShpDic, SHP},
        {&ResMdlData::toResTexNameToTexPlttInfoDic, TEXINFO},
        {&ResMdlData::toResPlttNameToTexPlttInfoDic, TEXINFO},
    };
    for (const DicKind& k : kDics) {
        s32 ofs = m->*k.ofs;
        if (ofs == 0) {
            continue;
        }
        ResDicData* d = SwapDic(At(m, ofs));
        if (d == NULL) {
            continue;
        }
        for (u32 i = 1; i <= d->numData; i++) {
            u8* e = At(d, d->data[i].ofsData);
            switch (k.kind) {
            case BYTECODE: break; // byte stream
            case NODE: SwapNode(e); break;
            case POS: SwapVtx(e, 0); break;
            case NRM: SwapVtx(e, 1); break;
            case CLR: SwapVtx(e, 2); break;
            case TEXCOORD: SwapVtx(e, 3); break;
            case MAT: SwapMat(e); break;
            case TEV: SwapTev(e); break;
            case SHP: SwapShp(e); break;
            case TEXINFO: SwapTexPlttInfoOffset(e); break;
            }
        }
    }
}

void Converter::SwapTex(u8* p) {
    if (!InFile(p, sizeof(ResTexData)) || !First(p)) {
        return;
    }
    ResTexData* t = reinterpret_cast<ResTexData*>(p);
    mTextures++;
    Swap32(&t->header.size);
    Swap32n(&t->revision, 5); // revision, toResFileData, toTexData, name, flag
    Swap16(&t->width);
    Swap16(&t->height);
    Swap32(&t->fmt);
    Swap32(&t->mipmap_level);
    SwapF32n(&t->min_lod, 2);
    Swap32(&t->original_path);
    Swap32(&t->toResUserData);
    SwapName(t, t->name);
}

void Converter::SwapPltt(u8* p) {
    if (!InFile(p, sizeof(ResPlttData)) || !First(p)) {
        return;
    }
    ResPlttData* t = reinterpret_cast<ResPlttData*>(p);
    mPalettes++;
    Swap32(&t->header.size);
    Swap32n(&t->revision, 5); // revision, toResFileData, toPlttData, name, fmt
    Swap16(&t->numEntries);
    Swap16(&t->PADDING_0x1E);
    Swap32(&t->original_path);
    Swap32(&t->toResUserData);
    SwapName(t, t->name);
}

void Converter::Run() {
    ResFileData* f = reinterpret_cast<ResFileData*>(mFile);
    Swap32(&f->dict.header.size);
    ResDicData* root = SwapDic(&f->dict.topLevel);
    if (root == NULL) {
        return;
    }
    for (u32 i = 1; i <= root->numData; i++) {
        const ResDicNodeData& n = root->data[i];
        const char* folder = n.ofsString != 0 ? reinterpret_cast<const char*>(At(root, n.ofsString)) : "";
        ResDicData* d = SwapDic(At(root, n.ofsData));
        if (d == NULL) {
            continue;
        }
        void (Converter::*fn)(u8*) = NULL;
        if (std::strcmp(folder, "3DModels(NW4R)") == 0) {
            fn = &Converter::SwapMdl;
        } else if (std::strcmp(folder, "Textures(NW4R)") == 0) {
            fn = &Converter::SwapTex;
        } else if (std::strcmp(folder, "Palettes(NW4R)") == 0) {
            fn = &Converter::SwapPltt;
        } else {
            port_log("g3d: brres folder %s (%lu entries) not converted\n", folder,
                     static_cast<unsigned long>(d->numData));
            continue;
        }
        for (u32 k = 1; k <= d->numData; k++) {
            (this->*fn)(At(d, d->data[k].ofsData));
        }
    }
}

} // namespace

namespace port {

void SwapResFileToNative(void* file) {
    ut::BinaryFileHeader* h = static_cast<ut::BinaryFileHeader*>(file);
    if (h == NULL || h->byteOrder != 0xFFFE) {
        return; // already native (or not a big-endian file)
    }
    Swap16(&h->byteOrder);
    Swap16(&h->version);
    Swap32(&h->fileSize);
    Swap16(&h->headerSize);
    Swap16(&h->dataBlocks);

    Converter c(static_cast<u8*>(file), h->fileSize);
    c.Run();
    port_log("g3d: brres %lu bytes -> native: %d models (%d materials, %d shapes, %d vertex arrays), "
             "%d textures, %d palettes%s\n",
             static_cast<unsigned long>(h->fileSize), c.mModels, c.mMaterials, c.mShapes, c.mVtxArrays,
             c.mTextures, c.mPalettes, c.mOk ? "" : " (with errors)");
}

} // namespace port
} // namespace g3d
} // namespace nw4r
