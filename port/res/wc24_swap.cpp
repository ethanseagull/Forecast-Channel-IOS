// In-place big-endian -> native conversion of the WiiConnect24 forecast files
// (forecast.bin, short.bin; structures in forecast-decomp
// include/channel/ForecastData.h), done once after download.
//
// The CRC in the header covers the file's bytes, so it is checked on the
// original big-endian data, then recomputed over the converted bytes: the
// channel's own check (ForecastCheck.cpp) then still means "the file arrived
// intact". Strings are UTF-16BE and are found through the offsets that point
// at them; a bitmap makes sure no 16-bit unit is swapped twice.
#include <string.h>

#include <vector>

#include <revolution/NET/netcrc.h>
#include <types.h>

#include <port_log.h>
#include <port_res.h>

using port::res::ReadBE32;
using port::res::Swap16;
using port::res::Swap16n;
using port::res::Swap32;
using port::res::Swap32n;

namespace {

struct Swapper {
    u8* base;
    u32 size;
    std::vector<bool> doneText; // per 16-bit unit

    bool Range(u32 offset, u64 bytes) const { return offset <= size && bytes <= size - offset; }

    // A null-terminated UTF-16 string at offset (0 = none).
    void Text(u32 offset) {
        if (offset == 0 || (offset & 1) || offset >= size) {
            return;
        }
        for (u32 o = offset; o + 2 <= size; o += 2) {
            if (doneText[o / 2]) {
                return;
            }
            doneText[o / 2] = true;
            Swap16(base + o);
            u16 c;
            memcpy(&c, base + o, 2);
            if (c == 0) {
                return;
            }
        }
    }

    u32 U32(u32 offset) const {
        u32 v;
        memcpy(&v, base + offset, 4);
        return v;
    }
};

// DayForecast (0x1C bytes): mWeather, mWeatherParts[4] are u16; the rest bytes.
void SwapDay(u8* p) { Swap16n(p, 5); }

bool SwapForecast(Swapper& s) {
    if (s.size < 0x58) {
        return false;
    }
    u8* h = s.base;
    Swap32n(h, 5);        // version, size, CRC, open time, time
    Swap32n(h + 0x1C, 15); // message offset, then 7 (count, offset) pairs
    s.Text(s.U32(0x1C));

    // Forecasts (0x80 bytes: ids/times u32 x3, 2 days, 7 x WeekForecast).
    {
        u32 n = s.U32(0x20), off = s.U32(0x24);
        if (!s.Range(off, (u64)n * 0x80)) {
            return false;
        }
        for (u32 i = 0; i < n; i++) {
            u8* e = h + off + i * 0x80;
            Swap32n(e, 3);
            SwapDay(e + 0x10);
            SwapDay(e + 0x2C);
            for (int w = 0; w < 7; w++) {
                Swap16(e + 0x48 + w * 8);
            }
        }
    }
    // Summaries (0x48 bytes: id, time u32, unk8[4] bytes, 2 days).
    {
        u32 n = s.U32(0x28), off = s.U32(0x2C);
        if (!s.Range(off, (u64)n * 0x48)) {
            return false;
        }
        for (u32 i = 0; i < n; i++) {
            u8* e = h + off + i * 0x48;
            Swap32n(e, 2);
            SwapDay(e + 0x10);
            SwapDay(e + 0x2C);
        }
    }
    // Weather types (8 bytes: code, icon u16; text offset u32).
    {
        u32 n = s.U32(0x30), off = s.U32(0x34);
        if (!s.Range(off, (u64)n * 8)) {
            return false;
        }
        for (u32 i = 0; i < n; i++) {
            u8* e = h + off + i * 8;
            Swap16n(e, 2);
            Swap32(e + 4);
            s.Text(s.U32(off + i * 8 + 4));
        }
    }
    // UV / laundry / pollen indices (8 bytes: code u8, 3 bytes, text offset).
    for (u32 t = 0; t < 3; t++) {
        u32 n = s.U32(0x38 + t * 8), off = s.U32(0x3C + t * 8);
        if (!s.Range(off, (u64)n * 8)) {
            return false;
        }
        for (u32 i = 0; i < n; i++) {
            u8* e = h + off + i * 8;
            Swap32(e + 4);
            s.Text(s.U32(off + i * 8 + 4));
        }
    }
    // Places (0x18 bytes: id, name, region, country offsets u32; lon, lat u16).
    {
        u32 n = s.U32(0x50), off = s.U32(0x54);
        if (!s.Range(off, (u64)n * 0x18)) {
            return false;
        }
        for (u32 i = 0; i < n; i++) {
            u8* e = h + off + i * 0x18;
            Swap32n(e, 4);
            Swap16n(e + 0x10, 2);
            for (int k = 1; k <= 3; k++) {
                s.Text(s.U32(off + i * 0x18 + k * 4));
            }
        }
    }
    return true;
}

bool SwapShort(Swapper& s) {
    if (s.size < 0x24) {
        return false;
    }
    u8* h = s.base;
    Swap32n(h, 3);       // version, size, CRC
    Swap32n(h + 0x10, 2); // time, country (0xC is read as a byte)
    Swap32n(h + 0x1C, 2); // entry count, offset
    u32 n = s.U32(0x1C), off = s.U32(0x20);
    // ShortEntry (0x18 bytes: id, time u32, unk8[4] bytes, weather u16, bytes).
    if (!s.Range(off, (u64)n * 0x18)) {
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        u8* e = h + off + i * 0x18;
        Swap32n(e, 2);
        Swap16(e + 0xC);
    }
    return true;
}

} // namespace

// Converts a downloaded, decompressed forecast file (taskId 3: forecast.bin,
// 4: short.bin). Returns false if the file is corrupt; it is then left as is
// (still big-endian), and the channel's own checks reject it.
extern "C" BOOL PortSwapWC24File(u16 taskId, void* data, u32 size) {
    u8* p = static_cast<u8*>(data);
    if (size < 0x24) {
        port_log("wc24: file %u too small (%u bytes)\n", taskId, size);
        return FALSE;
    }
    u32 crc = ReadBE32(p + 8);
    if (ReadBE32(p + 4) != size || NETCalcCRC32(p + 0xC, size - 0xC) != crc) {
        port_log("wc24: file %u: size or CRC mismatch\n", taskId);
        return FALSE;
    }
    // Work on a copy, so a structural error leaves the original untouched.
    std::vector<u8> copy(p, p + size);
    Swapper s;
    s.base = copy.data();
    s.size = size;
    s.doneText.assign(size / 2 + 1, false);
    bool ok = taskId == 3 ? SwapForecast(s) : taskId == 4 ? SwapShort(s) : false;
    if (!ok) {
        port_log("wc24: file %u: bad table offsets\n", taskId);
        return FALSE;
    }
    u32 native = NETCalcCRC32(s.base + 0xC, size - 0xC);
    memcpy(s.base + 8, &native, 4);
    memcpy(p, s.base, size);
    return TRUE;
}
