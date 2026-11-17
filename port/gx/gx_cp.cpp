// The GX command processor: parses the command stream (immediate batches and
// display lists) into register state and decoded primitives.
//
// Stream data is big-endian. Vertex arrays referenced by index are native
// (little-endian) unless they lie in a region registered as big-endian
// (resources kept in the Wii's byte order, such as model files).
#include <string.h>

#include <vector>

#include "gx_internal.h"
#include <port_gx.h>
#include <port_log.h>

GXRegs gx;

// --- big-endian regions --------------------------------------------------------

#define MAX_BE_REGIONS 32
static struct {
    uintptr_t lo, hi;
} sBeRegions[MAX_BE_REGIONS];
static int sBeRegionCount;

extern "C" void port_gx_register_be(const void* p, u32 size) {
    for (int i = 0; i < sBeRegionCount; i++) {
        if (sBeRegions[i].lo == (uintptr_t)p) {
            sBeRegions[i].hi = (uintptr_t)p + size;
            return;
        }
    }
    if (sBeRegionCount == MAX_BE_REGIONS) {
        port_log("gx: too many big-endian regions\n");
        return;
    }
    sBeRegions[sBeRegionCount].lo = (uintptr_t)p;
    sBeRegions[sBeRegionCount].hi = (uintptr_t)p + size;
    sBeRegionCount++;
}

extern "C" void port_gx_unregister_be(const void* p) {
    for (int i = 0; i < sBeRegionCount; i++) {
        if (sBeRegions[i].lo == (uintptr_t)p) {
            sBeRegions[i] = sBeRegions[--sBeRegionCount];
            return;
        }
    }
}

static bool is_be(const u8* p) {
    uintptr_t a = (uintptr_t)p;
    for (int i = 0; i < sBeRegionCount; i++) {
        if (a >= sBeRegions[i].lo && a < sBeRegions[i].hi) {
            return true;
        }
    }
    return false;
}

bool gx_addr_ok(const void* p, u32 size) {
    return p != NULL && port_mem_readable(p, size);
}

// --- register writes --------------------------------------------------------------

static void load_tlut(void) {
    // BP 0x64: source address >> 5. BP 0x65: TMEM line (512-byte units above
    // 0x80000) and the number of 32-byte lines.
    const u8* src = gx_mem(gx_bits(gx.bp[0x64], 0, 24) << 5);
    u32 tmem = gx_bits(gx.bp[0x65], 0, 10) << 9;
    u32 bytes = gx_bits(gx.bp[0x65], 10, 11) * 32;
    if (!gx_addr_ok(src, bytes)) {
        port_log("gx: TLUT source unmapped (%08x)\n", gx_bits(gx.bp[0x64], 0, 24) << 5);
        return;
    }
    if (tmem + bytes > 0x80000) {
        port_log("gx: TLUT load out of range (%lx + %lu)\n", (unsigned long)tmem, (unsigned long)bytes);
        return;
    }
    // TLUTs are copied as the GPU would; the decoder reads them big-endian
    // (palettes come from TPL files, which keep their byte order).
    memcpy(gx.tmemHi + tmem, src, bytes);
}

static void write_tev_reg(u32 reg, u32 value) {
    // 0xE0 + 2n: R (bits 0-10) and A (12-22); 0xE1 + 2n: B and G. Bit 23
    // selects the konstant colour instead of the register.
    int n = (reg - 0xE0) / 2;
    bool hi = (reg & 1) != 0;
    bool konst = (value >> 23) & 1;
    s16 c0 = (s16)(gx_bits(value, 0, 11) << 5) >> 5;
    s16 c1 = (s16)(gx_bits(value, 12, 11) << 5) >> 5;
    if (konst) {
        if (hi) {
            gx.konst[n][2] = (u8)c0;
            gx.konst[n][1] = (u8)c1;
        } else {
            gx.konst[n][0] = (u8)c0;
            gx.konst[n][3] = (u8)c1;
        }
    } else {
        if (hi) {
            gx.tevReg[n][2] = c0;
            gx.tevReg[n][1] = c1;
        } else {
            gx.tevReg[n][0] = c0;
            gx.tevReg[n][3] = c1;
        }
    }
}

static void write_bp(u32 word) {
    u32 reg = word >> 24;
    u32 value = word & 0xFFFFFF;

    if (reg == 0xFE) {
        gx.bpMask = value;
        return;
    }
    if (gx.bpMask != 0xFFFFFF) {
        value = (gx.bp[reg] & ~gx.bpMask) | (value & gx.bpMask);
        gx.bpMask = 0xFFFFFF;
    }
    gx.bp[reg] = value;

    switch (reg) {
    case 0x52:
        gx_efb_copy(value);
        break;
    case 0x65:
        load_tlut();
        break;
    case 0x66:
        gx_tex_invalidate();
        break;
    default:
        if (reg >= 0xE0 && reg <= 0xE7) {
            write_tev_reg(reg, value);
        }
        break;
    }
}

static void write_xf(u32 addr, u32 value) {
    if (addr < 0x1000) {
        gx.xfMem[addr] = value;
    } else if (addr < 0x1100) {
        gx.xfReg[addr - 0x1000] = value;
    }
}

static u32 read_array_u32(const u8* p) {
    return is_be(p) ? gx_be32(p) : *(const u32*)p;
}

static void load_indexed(u32 cmd, u32 word) {
    // Indexed XF load from array A-D (CP arrays 12-15): index (top 16 bits),
    // count - 1 (bits 12-15), XF address (bits 0-11).
    int array = 12 + ((cmd - 0x20) >> 3);
    u32 index = word >> 16;
    u32 count = gx_bits(word, 12, 4) + 1;
    u32 addr = gx_bits(word, 0, 12);
    const u8* src = gx_mem(gx.cp[0xA0 + array]) + index * gx.cp[0xB0 + array];
    for (u32 i = 0; i < count; i++) {
        write_xf(addr + i, read_array_u32(src + i * 4));
    }
}

// --- vertex formats -----------------------------------------------------------------

enum { ATTR_NONE, ATTR_DIRECT, ATTR_INDEX8, ATTR_INDEX16 };

struct AttrDesc {
    u8 mode;
    u8 count; // components (or the colour format for colours)
    u8 type;
    u8 frac;
};

struct VtxFormat {
    bool pnMtx;
    u8 texMtx; // bit per texcoord
    AttrDesc pos, nrm, clr[2], tex[8];
    bool nbt, nrmIndex3;
    u32 size; // bytes per vertex in the stream
};

static const u8 kCompSize[8] = {1, 1, 2, 2, 4, 0, 0, 0};
static const u8 kClrSize[8] = {2, 3, 4, 2, 3, 4, 0, 0};

static void tex_fmt(int i, u32 a, u32 b, u32 c, AttrDesc* d) {
    u32 e, f, fr;
    switch (i) {
    case 0: e = a >> 21; f = a >> 22; fr = a >> 25; break;
    case 1: e = b; f = b >> 1; fr = b >> 4; break;
    case 2: e = b >> 9; f = b >> 10; fr = b >> 13; break;
    case 3: e = b >> 18; f = b >> 19; fr = b >> 22; break;
    case 4: e = b >> 27; f = b >> 28; fr = c; break;
    case 5: e = c >> 5; f = c >> 6; fr = c >> 9; break;
    case 6: e = c >> 14; f = c >> 15; fr = c >> 18; break;
    default: e = c >> 23; f = c >> 24; fr = c >> 27; break;
    }
    d->count = (e & 1) ? 2 : 1;
    d->type = f & 7;
    d->frac = fr & 31;
}

static u32 attr_size(const AttrDesc& d, u32 directBytes) {
    switch (d.mode) {
    case ATTR_DIRECT: return directBytes;
    case ATTR_INDEX8: return 1;
    case ATTR_INDEX16: return 2;
    default: return 0;
    }
}

static void build_format(int fmt, VtxFormat* vf) {
    u32 lo = gx.cp[0x50], hi = gx.cp[0x60];
    u32 a = gx.cp[0x70 + fmt], b = gx.cp[0x80 + fmt], c = gx.cp[0x90 + fmt];

    vf->pnMtx = lo & 1;
    vf->texMtx = (u8)gx_bits(lo, 1, 8);
    vf->pos.mode = (u8)gx_bits(lo, 9, 2);
    vf->nrm.mode = (u8)gx_bits(lo, 11, 2);
    vf->clr[0].mode = (u8)gx_bits(lo, 13, 2);
    vf->clr[1].mode = (u8)gx_bits(lo, 15, 2);
    for (int i = 0; i < 8; i++) {
        vf->tex[i].mode = (u8)gx_bits(hi, i * 2, 2);
        tex_fmt(i, a, b, c, &vf->tex[i]);
    }

    vf->pos.count = (a & 1) ? 3 : 2;
    vf->pos.type = (u8)gx_bits(a, 1, 3);
    vf->pos.frac = (u8)gx_bits(a, 4, 5);
    vf->nbt = (a >> 9) & 1;
    vf->nrm.count = 3;
    vf->nrm.type = (u8)gx_bits(a, 10, 3);
    vf->nrm.frac = vf->nrm.type == 1 ? 6 : vf->nrm.type == 3 ? 14 : 0;
    vf->nrmIndex3 = (a >> 31) & 1;
    vf->clr[0].type = (u8)gx_bits(a, 14, 3);
    vf->clr[1].type = (u8)gx_bits(a, 18, 3);

    u32 size = 0;
    if (vf->pnMtx) {
        size++;
    }
    for (int i = 0; i < 8; i++) {
        if (vf->texMtx & (1 << i)) {
            size++;
        }
    }
    size += attr_size(vf->pos, vf->pos.count * kCompSize[vf->pos.type]);
    u32 nrmVecs = vf->nbt ? 3 : 1;
    if (vf->nrm.mode >= ATTR_INDEX8 && vf->nbt && vf->nrmIndex3) {
        size += attr_size(vf->nrm, 0) * 3;
    } else {
        size += attr_size(vf->nrm, nrmVecs * 3 * kCompSize[vf->nrm.type]);
    }
    for (int i = 0; i < 2; i++) {
        size += attr_size(vf->clr[i], kClrSize[vf->clr[i].type]);
    }
    for (int i = 0; i < 8; i++) {
        size += attr_size(vf->tex[i], vf->tex[i].count * kCompSize[vf->tex[i].type]);
    }
    vf->size = size;
}

static f32 read_comp(const u8* p, u32 type, u32 frac, bool be) {
    f32 scale = 1.0f / (f32)(1u << frac);
    switch (type) {
    case 0: return (f32)p[0] * scale;
    case 1: return (f32)(s8)p[0] * scale;
    case 2: return (f32)(be ? gx_be16(p) : *(const u16*)p) * scale;
    case 3: return (f32)(s16)(be ? gx_be16(p) : *(const u16*)p) * scale;
    default: {
        union {
            u32 u;
            f32 f;
        } x;
        x.u = be ? gx_be32(p) : *(const u32*)p;
        return x.f;
    }
    }
}

static void read_color(const u8* p, u32 type, bool be, u8 out[4]) {
    switch (type) {
    case 0: { // RGB565
        u32 v = be ? gx_be16(p) : *(const u16*)p;
        u32 r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        out[0] = (u8)(r << 3 | r >> 2);
        out[1] = (u8)(g << 2 | g >> 4);
        out[2] = (u8)(b << 3 | b >> 2);
        out[3] = 255;
        break;
    }
    case 1: // RGB8
    case 2: // RGBX8
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        out[3] = 255;
        break;
    case 3: { // RGBA4
        u32 v = be ? gx_be16(p) : *(const u16*)p;
        out[0] = (u8)((v >> 12) * 0x11);
        out[1] = (u8)(((v >> 8) & 15) * 0x11);
        out[2] = (u8)(((v >> 4) & 15) * 0x11);
        out[3] = (u8)((v & 15) * 0x11);
        break;
    }
    case 4: { // RGBA6
        u32 v = (u32)p[0] << 16 | (u32)p[1] << 8 | p[2];
        for (int i = 0; i < 4; i++) {
            u32 c = (v >> (18 - 6 * i)) & 63;
            out[i] = (u8)(c << 2 | c >> 4);
        }
        break;
    }
    default: // RGBA8
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        out[3] = p[3];
        break;
    }
}

// Returns the attribute's data and whether it is big-endian; advances *p past
// what the stream holds for it.
static const u8* attr_data(const AttrDesc& d, int array, u32 directBytes, const u8** p, bool* be) {
    const u8* data;
    u32 index;
    switch (d.mode) {
    case ATTR_DIRECT:
        data = *p;
        *p += directBytes;
        *be = true;
        return data;
    case ATTR_INDEX8:
        index = **p;
        *p += 1;
        break;
    default:
        index = gx_be16(*p);
        *p += 2;
        break;
    }
    data = gx_mem(gx.cp[0xA0 + array]) + index * gx.cp[0xB0 + array];
    if (!gx_addr_ok(data, 4)) {
        static int sBad;
        if (sBad++ < 8) {
            port_log("gx: vertex array %d at %08lx (base %08lx, stride %lu, index %lu) unmapped\n", array,
                     (unsigned long)(uintptr_t)data, (unsigned long)gx.cp[0xA0 + array],
                     (unsigned long)gx.cp[0xB0 + array], (unsigned long)index);
        }
        static const u8 kZero[64] = {0};
        *be = false;
        return kZero;
    }
    *be = is_be(data);
    return data;
}

static void decode_vertex(const VtxFormat& vf, const u8* p, GXVertexIn* v) {
    bool be;
    v->hasPnMtx = vf.pnMtx;
    v->hasTexMtx = vf.texMtx;
    v->hasClr = 0;
    v->hasNrm = 0;
    if (vf.pnMtx) {
        v->pnMtx = *p++;
    }
    for (int i = 0; i < 8; i++) {
        if (vf.texMtx & (1 << i)) {
            v->texMtx[i] = *p++;
        }
    }

    {
        u32 cs = kCompSize[vf.pos.type];
        const u8* d = attr_data(vf.pos, 0, vf.pos.count * cs, &p, &be);
        v->pos[0] = read_comp(d, vf.pos.type, vf.pos.frac, be);
        v->pos[1] = read_comp(d + cs, vf.pos.type, vf.pos.frac, be);
        v->pos[2] = vf.pos.count == 3 ? read_comp(d + 2 * cs, vf.pos.type, vf.pos.frac, be) : 0.0f;
    }

    // The binormal and tangent of the last NBT normal: the hardware keeps
    // them for vertices that don't have their own.
    static f32 sBinormal[3], sTangent[3];
    if (vf.nrm.mode != ATTR_NONE) {
        u32 cs = kCompSize[vf.nrm.type];
        u32 vecs = vf.nbt ? 3 : 1;
        // Vector k (0 normal, 1 binormal, 2 tangent) of an NBT normal.
        const u8* d[3];
        bool vbe[3];
        if (vf.nrm.mode >= ATTR_INDEX8 && vf.nbt && vf.nrmIndex3) {
            // Three indices, one per vector: vector k is at index k's
            // element, k * 3 components in (as Dolphin's VertexLoader_Normal
            // reads it).
            for (u32 k = 0; k < 3; k++) {
                d[k] = attr_data(vf.nrm, 1, 0, &p, &vbe[k]) + k * 3 * cs;
            }
        } else {
            d[0] = attr_data(vf.nrm, 1, vecs * 3 * cs, &p, &vbe[0]);
            for (u32 k = 1; k < 3; k++) {
                d[k] = d[0] + k * 3 * cs;
                vbe[k] = vbe[0];
            }
        }
        for (int i = 0; i < 3; i++) {
            v->nrm[i] = read_comp(d[0] + i * cs, vf.nrm.type, vf.nrm.frac, vbe[0]);
        }
        if (vf.nbt) {
            // Direct data is inside the stream (run_draw checked its size);
            // attr_data checked only 4 bytes at an indexed element.
            bool ok = vf.nrm.mode == ATTR_DIRECT || (gx_addr_ok(d[1], 3 * cs) && gx_addr_ok(d[2], 3 * cs));
            for (int i = 0; i < 3; i++) {
                sBinormal[i] = ok ? read_comp(d[1] + i * cs, vf.nrm.type, vf.nrm.frac, vbe[1]) : 0.0f;
                sTangent[i] = ok ? read_comp(d[2] + i * cs, vf.nrm.type, vf.nrm.frac, vbe[2]) : 0.0f;
            }
        }
        v->hasNrm = 1;
    }
    for (int i = 0; i < 3; i++) {
        v->binormal[i] = sBinormal[i];
        v->tangent[i] = sTangent[i];
    }

    for (int c = 0; c < 2; c++) {
        if (vf.clr[c].mode != ATTR_NONE) {
            const u8* d = attr_data(vf.clr[c], 2 + c, kClrSize[vf.clr[c].type], &p, &be);
            read_color(d, vf.clr[c].type, be, v->clr[c]);
            v->hasClr |= 1 << c;
        }
    }

    for (int t = 0; t < 8; t++) {
        const AttrDesc& td = vf.tex[t];
        if (td.mode == ATTR_NONE) {
            continue;
        }
        u32 cs = kCompSize[td.type];
        const u8* d = attr_data(td, 4 + t, td.count * cs, &p, &be);
        v->tex[t][0] = read_comp(d, td.type, td.frac, be);
        v->tex[t][1] = td.count == 2 ? read_comp(d + cs, td.type, td.frac, be) : 0.0f;
    }
}

// --- the parser -------------------------------------------------------------------

// Decoded vertices of the current primitive; grows to the largest one seen.
static std::vector<GXVertexIn> sVerts;
static int sDepth;

static bool run_draw(u8 cmd, const u8** pp, const u8* end) {
    const u8* p = *pp;
    if (end - p < 2) {
        return false;
    }
    u32 count = gx_be16(p);
    p += 2;
    VtxFormat vf;
    build_format(cmd & 7, &vf);
    if ((u32)(end - p) < count * vf.size) {
        port_log("gx: primitive %02x runs past the stream (%lu x %lu bytes)\n", cmd,
                 (unsigned long)count, (unsigned long)vf.size);
        return false;
    }
    if (sVerts.size() < count) {
        sVerts.resize(count);
    }
    for (u32 i = 0; i < count; i++) {
        decode_vertex(vf, p, &sVerts[i]);
        p += vf.size;
    }
    *pp = p;
    if (count != 0) {
        gx_draw(cmd & 0xF8, sVerts.data(), count);
    }
    return true;
}

void gx_cp_run(const u8* data, u32 size) {
    const u8* p = data;
    const u8* end = data + size;

    if (gx.tmemHi == NULL) {
        gx.tmemHi = new u8[0x80000];
        gx.bpMask = 0xFFFFFF;
    }
    if (++sDepth > 4) {
        port_log("gx: display lists nested too deep\n");
        sDepth--;
        return;
    }

    while (p < end) {
        u8 cmd = *p++;
        switch (cmd) {
        case 0x00: // NOP
        case 0x48: // invalidate vertex cache
            break;
        case 0x08: // CP register
            if (end - p < 5) {
                goto truncated;
            }
            gx.cp[p[0]] = gx_be32(p + 1);
            p += 5;
            break;
        case 0x10: { // XF registers
            if (end - p < 4) {
                goto truncated;
            }
            u32 hdr = gx_be32(p);
            u32 count = (hdr >> 16) + 1;
            u32 addr = hdr & 0xFFFF;
            p += 4;
            if ((u32)(end - p) < count * 4) {
                goto truncated;
            }
            for (u32 i = 0; i < count; i++) {
                write_xf(addr + i, gx_be32(p));
                p += 4;
            }
            break;
        }
        case 0x20:
        case 0x28:
        case 0x30:
        case 0x38:
            if (end - p < 4) {
                goto truncated;
            }
            load_indexed(cmd, gx_be32(p));
            p += 4;
            break;
        case 0x40: { // call display list
            if (end - p < 8) {
                goto truncated;
            }
            // GXCallDisplayList writes the list as a GX address
            // (port_gx_write_p), not a host pointer.
            const u8* list = gx_mem(gx_be32(p));
            u32 listSize = gx_be32(p + 4);
            p += 8;
            if (!gx_addr_ok(list, listSize)) {
                port_log("gx: display list %08lx (%lu bytes) unmapped\n", (unsigned long)gx_be32(p - 8),
                         (unsigned long)listSize);
                break;
            }
            gx_cp_run(list, listSize);
            break;
        }
        case 0x61: // BP register
            if (end - p < 4) {
                goto truncated;
            }
            write_bp(gx_be32(p));
            p += 4;
            break;
        default:
            if (cmd >= 0x80 && cmd < 0xC0) {
                if (!run_draw(cmd, &p, end)) {
                    goto truncated;
                }
                break;
            }
            port_log("gx: unknown command %02x at +%lu of %lu, rest dropped\n", cmd,
                     (unsigned long)(p - 1 - data), (unsigned long)size);
            sDepth--;
            return;
        }
    }
    sDepth--;
    return;

truncated:
    port_log("gx: command %02x truncated at +%lu of %lu\n", p[-1], (unsigned long)(p - data),
             (unsigned long)size);
    sDepth--;
}
