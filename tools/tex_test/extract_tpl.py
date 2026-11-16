"""Pulls every texture out of the channel's content into TPL files for tex_test.

    py -I tools/tex_test/extract_tpl.py <content dir> <output dir>

Walks the content directory, unpacks LZ77 (type 0x10/0x11) and U8 archives
(recursively), and writes:
  - every .tpl as is,
  - every TEX0 in a BRRES (with its PLT0 palette for CI formats),
  - every TGLP font sheet in a BRFNT,
as TPL files, so one decoder test covers them all. The output holds Nintendo
data: keep it out of the repository.
"""

import os
import re
import struct
import sys

TPL_MAGIC = b"\x00\x20\xaf\x30"
U8_MAGIC = b"\x55\xaa\x38\x2d"


def be16(d, o):
    return struct.unpack_from(">H", d, o)[0]


def be32(d, o):
    return struct.unpack_from(">I", d, o)[0]


def lz77_decompress(d):
    kind = d[0]
    size = d[1] | d[2] << 8 | d[3] << 16
    pos = 4
    if size == 0:
        size = struct.unpack_from("<I", d, 4)[0]
        pos = 8
    out = bytearray()
    while len(out) < size:
        flags = d[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> bit):
                out.append(d[pos])
                pos += 1
                continue
            b0 = d[pos]
            if kind == 0x10:
                b1 = d[pos + 1]
                pos += 2
                length = (b0 >> 4) + 3
                disp = ((b0 & 15) << 8 | b1) + 1
            else:
                ind = b0 >> 4
                if ind == 0:
                    b1, b2 = d[pos + 1], d[pos + 2]
                    pos += 3
                    length = ((b0 & 15) << 4 | b1 >> 4) + 0x11
                    disp = ((b1 & 15) << 8 | b2) + 1
                elif ind == 1:
                    b1, b2, b3 = d[pos + 1], d[pos + 2], d[pos + 3]
                    pos += 4
                    length = ((b0 & 15) << 12 | b1 << 4 | b2 >> 4) + 0x111
                    disp = ((b2 & 15) << 8 | b3) + 1
                else:
                    b1 = d[pos + 1]
                    pos += 2
                    length = ind + 1
                    disp = ((b0 & 15) << 8 | b1) + 1
            for _ in range(length):
                out.append(out[-disp])
    return bytes(out[:size])


def maybe_decompress(name, d):
    if len(d) >= 8 and d[0] in (0x10, 0x11) and ("LZ77" in name or name.upper().endswith(".LZ")):
        return lz77_decompress(d)
    return d


def u8_files(d):
    """Yields (path, bytes) for every file in a U8 archive."""
    root = be32(d, 4)
    count = be32(d, root + 8)
    strings = root + count * 12
    dirs = [("", count)]  # (path, end index)
    for i in range(1, count):
        while dirs and i >= dirs[-1][1]:
            dirs.pop()
        node = root + i * 12
        kind = d[node]
        name_off = be32(d, node) & 0xFFFFFF
        end = d.index(b"\0", strings + name_off)
        name = d[strings + name_off:end].decode("ascii", "replace")
        path = (dirs[-1][0] + "/" + name) if dirs else name
        if kind == 1:
            dirs.append((path, be32(d, node + 8)))
        else:
            off, size = be32(d, node + 4), be32(d, node + 8)
            yield path, d[off:off + size]


def make_tpl(images):
    """images: list of (w, h, fmt, data, palette) with palette None or
    (tlut fmt, entries, data). Returns TPL bytes."""
    n = len(images)
    out = bytearray(TPL_MAGIC + struct.pack(">II", n, 12))
    table = len(out)
    out += bytes(8 * n)
    for i, (w, h, fmt, data, pal) in enumerate(images):
        pal_hdr = 0
        if pal is not None:
            pal_hdr = len(out)
            out += bytes(12)
        img_hdr = len(out)
        out += bytes(36)
        while len(out) % 32:
            out.append(0)
        data_off = len(out)
        out += data
        if pal is not None:
            while len(out) % 32:
                out.append(0)
            pal_off = len(out)
            out += pal[2]
            struct.pack_into(">HBBII", out, pal_hdr, pal[1], 0, 0, pal[0], pal_off)
        struct.pack_into(">HHII", out, img_hdr, h, w, fmt, data_off)
        struct.pack_into(">II", out, table + i * 8, img_hdr, pal_hdr)
    return bytes(out)


def brres_name(d, sec, name_off_field):
    off = be32(d, sec + name_off_field)
    if off == 0 or sec + off >= len(d):
        return None
    end = d.index(b"\0", sec + off)
    return d[sec + off:end].decode("ascii", "replace")


def brres_textures(d):
    """TEX0 sections (level 0 only), with their PLT0 palettes by name."""
    palettes = {}
    for m in re.finditer(b"PLT0", d):
        s = m.start()
        if s % 4 or s + 0x40 > len(d):
            continue
        name = brres_name(d, s, 0x14)
        fmt, entries = be32(d, s + 0x18), be16(d, s + 0x1C)
        data = s + be32(d, s + 0x10)
        if name is None or fmt > 2 or entries == 0:
            continue
        palettes[name] = (fmt, entries, d[data:data + entries * 2])
    for m in re.finditer(b"TEX0", d):
        s = m.start()
        if s % 4 or s + 0x40 > len(d):
            continue
        name = brres_name(d, s, 0x14)
        w, h, fmt = be16(d, s + 0x1C), be16(d, s + 0x1E), be32(d, s + 0x20)
        if name is None or not 0 < w <= 1024 or not 0 < h <= 1024 or fmt > 14:
            continue
        data = s + be32(d, s + 0x10)
        pal = palettes.get(name) if fmt in (8, 9, 10) else None
        if fmt in (8, 9, 10) and pal is None:
            print("  %s: CI texture without a palette, skipped" % name)
            continue
        yield name, (w, h, fmt, d[data:data + be32(d, s + 4)], pal)


def rfnt_sheets(d):
    for m in re.finditer(b"TGLP", d):
        s = m.start()
        sheet_size = be32(d, s + 0x0C)
        num, fmt = be16(d, s + 0x10), be16(d, s + 0x12)
        w, h = be16(d, s + 0x18), be16(d, s + 0x1A)
        img = be32(d, s + 0x1C)
        for i in range(num):
            yield i, (w, h, fmt, d[img + i * sheet_size:img + (i + 1) * sheet_size], None)


def safe(path):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", path).strip("_")


def handle(path, d, out_dir, stats):
    name = path.rsplit("/", 1)[-1]
    d = maybe_decompress(name, d)
    if d[:4] == U8_MAGIC:
        for sub, data in u8_files(d):
            handle(path + "/" + sub, data, out_dir, stats)
    elif d[:4] == TPL_MAGIC:
        write(out_dir, path, d, stats)
    elif d[:4] == b"bres":
        images = list(brres_textures(d))
        for tex_name, img in images:
            write(out_dir, path + "/" + tex_name, make_tpl([img]), stats)
    elif d[:4] == b"RFNT":
        for i, img in rfnt_sheets(d):
            write(out_dir, path + "/sheet%d" % i, make_tpl([img]), stats)


def write(out_dir, path, data, stats):
    fn = safe(path)
    if not fn.endswith(".tpl"):
        fn += ".tpl"
    with open(os.path.join(out_dir, fn), "wb") as f:
        f.write(data)
    stats.append(fn)


def main():
    content, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    stats = []
    for dirpath, _, files in os.walk(content):
        for fn in sorted(files):
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, content).replace(os.sep, "/")
            with open(full, "rb") as f:
                d = f.read()
            try:
                handle(rel, d, out_dir, stats)
            except Exception as e:  # keep going over odd files
                print("  %s: %s" % (rel, e))
    print("%d TPL files written to %s" % (len(stats), out_dir))


if __name__ == "__main__":
    main()
