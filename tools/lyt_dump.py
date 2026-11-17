"""Dump the pane trees of the channel's NW4R layouts (RLYT).

    py -I tools\\lyt_dump.py <weather_layout.arc[.LZ]> [name-filter]

Reads an LZ77/LZ11-compressed or plain U8 archive (or a single .brlyt) and
prints, per layout: its size, then each pane indented by depth with its kind,
name, translate, size, scale, base position and (for text panes) the string.
Groups follow. Used to write the portrait pane overrides (port/channel/
portrait_layouts.cpp): no Nintendo data is stored in the repository.
"""
import struct
import sys


def lz_decompress(data):
    kind = data[0]
    size = data[1] | (data[2] << 8) | (data[3] << 16)
    pos = 4
    if size == 0:
        size = struct.unpack_from("<I", data, 4)[0]
        pos = 8
    out = bytearray()
    while len(out) < size:
        flags = data[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                if kind == 0x10:
                    b0, b1 = data[pos], data[pos + 1]
                    pos += 2
                    length = (b0 >> 4) + 3
                    disp = (((b0 & 0xF) << 8) | b1) + 1
                else:  # 0x11
                    b0 = data[pos]
                    ind = b0 >> 4
                    if ind == 0:
                        b1, b2 = data[pos + 1], data[pos + 2]
                        pos += 3
                        length = (((b0 & 0xF) << 4) | (b1 >> 4)) + 0x11
                        disp = (((b1 & 0xF) << 8) | b2) + 1
                    elif ind == 1:
                        b1, b2, b3 = data[pos + 1], data[pos + 2], data[pos + 3]
                        pos += 4
                        length = (((b0 & 0xF) << 12) | (b1 << 4) | (b2 >> 4)) + 0x111
                        disp = (((b2 & 0xF) << 8) | b3) + 1
                    else:
                        b1 = data[pos + 1]
                        pos += 2
                        length = ind + 1
                        disp = (((b0 & 0xF) << 8) | b1) + 1
                for _ in range(length):
                    out.append(out[-disp])
            else:
                out.append(data[pos])
                pos += 1
    return bytes(out[:size])


def u8_files(data):
    assert data[:4] == b"\x55\xAA\x38\x2D", "not a U8 archive"
    root_off, _, data_off = struct.unpack_from(">III", data, 4)
    total = struct.unpack_from(">I", data, root_off + 8)[0]
    str_base = root_off + total * 12
    files = []
    stack = []  # (end index, name)
    i = 1
    while i < total:
        while stack and i >= stack[-1][0]:
            stack.pop()
        t_name, off, size = struct.unpack_from(">III", data, root_off + i * 12)
        is_dir = t_name >> 24
        name_off = t_name & 0xFFFFFF
        end = data.index(b"\0", str_base + name_off)
        name = data[str_base + name_off:end].decode("ascii")
        path = "/".join([s[1] for s in stack] + [name])
        if is_dir:
            stack.append((size, name))
        else:
            files.append((path, data[off:off + size]))
        i += 1
    return files


def cstr(b):
    return b.split(b"\0", 1)[0].decode("latin-1")


BASE = ["TL", "TC", "TR", "CL", "C", "CR", "BL", "BC", "BR"]


def dump_rlyt(name, d):
    assert d[:4] == b"RLYT"
    nsec = struct.unpack_from(">H", d, 14)[0]
    pos = struct.unpack_from(">H", d, 12)[0]
    depth = 0
    in_group = 0
    print(f"=== {name}")
    for _ in range(nsec):
        magic = d[pos:pos + 4].decode("latin-1")
        size = struct.unpack_from(">I", d, pos + 4)[0]
        s = pos + 8
        if magic == "lyt1":
            w, h = struct.unpack_from(">ff", d, s + 4)
            print(f"  layout {w:g} x {h:g}, from centre {d[s]}")
        elif magic in ("pan1", "pic1", "txt1", "wnd1", "bnd1"):
            flag, basepos, alpha = d[s], d[s + 1], d[s + 2]
            pname = cstr(d[s + 4:s + 20])
            tx, ty, tz, rx, ry, rz, sx, sy, w, h = struct.unpack_from(">10f", d, s + 28)
            extra = ""
            if magic == "txt1":
                strbytes = struct.unpack_from(">H", d, s + 0x46)[0]
                stroff = struct.unpack_from(">I", d, s + 0x50)[0]
                fsx, fsy = struct.unpack_from(">ff", d, s + 0x5C)
                align = d[s + 0x4D]
                raw = d[pos + stroff:pos + stroff + strbytes]
                txt = raw.decode("utf-16-be", "replace").rstrip("\0").replace("\n", "\\n")
                extra = f' font {fsx:g}x{fsy:g} pos {BASE[d[s + 0x4C]] if d[s + 0x4C] < 9 else d[s + 0x4C]} "{txt}"'
            vis = ("" if flag & 1 else " HIDDEN") + (" LA" if flag & 4 else "")
            rot = f" rot {rz:g}" if rz else ""
            print(f"  {'  ' * depth}{magic[:3]} {pname:<16} t({tx:g},{ty:g}) size {w:g}x{h:g} s({sx:g},{sy:g}) "
                  f"{BASE[basepos] if basepos < 9 else basepos} a{alpha}{rot}{vis}{extra}")
        elif magic == "pas1":
            depth += 1
        elif magic == "pae1":
            depth -= 1
        elif magic == "grs1":
            in_group += 1
        elif magic == "gre1":
            in_group -= 1
        elif magic == "grp1":
            gname = cstr(d[s:s + 16])
            n = struct.unpack_from(">H", d, s + 16)[0]
            members = [cstr(d[s + 20 + 16 * i:s + 36 + 16 * i]) for i in range(n)]
            print(f"  {'  ' * in_group}group {gname}: {', '.join(members)}")
        pos += size


def main():
    path = sys.argv[1]
    flt = sys.argv[2] if len(sys.argv) > 2 else ""
    data = open(path, "rb").read()
    if data[0] in (0x10, 0x11) and data[:4] != b"\x55\xAA\x38\x2D":
        data = lz_decompress(data)
    if data[:4] == b"RLYT":
        dump_rlyt(path, data)
        return
    for name, blob in u8_files(data):
        if name.endswith(".brlyt") and flt in name:
            dump_rlyt(name, blob)
        elif not flt:
            print(f"--- {name} ({len(blob)} bytes)")


if __name__ == "__main__":
    main()
