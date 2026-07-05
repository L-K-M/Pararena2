#!/usr/bin/env python3
"""Build the Pararena 2 port asset pack from the original resource files.

Inputs (from the repository root):
  Pararena.project.r   -- DeRez text containing all app resources
  Para Sounds.bin      -- MacBinary file whose resource fork holds SMSD 26-28/100-105

Outputs (under port/assets/):
  pararena2.dat        -- little-endian pack consumed by the game at runtime
  preview/*.png|*.wav  -- human-checkable previews (not needed at runtime)

Pack layout:
  header:  "PAR2" u32 version u32 count
  toc:     count * { u32 fourcc, s32 id, u32 offset, u32 size, u32 w, u32 h, u32 flags }
  fourccs: 'PIX ' images (8bpp palette indices, or 0/1 for masks; flags bit0 = mask)
           'SND ' sounds (u16 priority, u16 loopFlag, then unsigned 8-bit PCM @22254.5Hz)
           'FORC'/'VERT'/'SPTS' raw big-endian originals (loader byte-swaps)
           'CLUT' 16 * {u8 r,g,b,a}
           'STR#' u16 count then Pascal strings (raw Mac Roman bytes)
"""
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REZ_PATH = os.path.join(ROOT, "Pararena.project.r")
SOUNDS_PATH = os.path.join(ROOT, "Para Sounds.bin")
OUT_DIR = os.path.join(ROOT, "port", "assets")
PREVIEW_DIR = os.path.join(OUT_DIR, "preview")

MASK_PICTS = {1020, 1021}  # sprite mask sheets: keep pixels as 0/1

# ---------------------------------------------------------------- DeRez parse

def parse_rez(path):
    """Return {(type, id): bytes} from a DeRez text file."""
    res = {}
    header_re = re.compile(r"^data '(.{4})' \((-?\d+)")
    with open(path, "r", encoding="latin-1") as f:
        cur = None
        hexparts = []
        for line in f:
            if cur is None:
                m = header_re.match(line)
                if m:
                    cur = (m.group(1), int(m.group(2)))
                    hexparts = []
            else:
                if line.startswith("};"):
                    res[cur] = bytes.fromhex("".join(hexparts))
                    cur = None
                else:
                    m = re.search(r'\$"([0-9A-Fa-f ]*)"', line)
                    if m:
                        hexparts.append(m.group(1).replace(" ", ""))
    return res


# ---------------------------------------------------------------- MacBinary + resource fork

def macbinary_resource_fork(path):
    with open(path, "rb") as f:
        hdr = f.read(128)
        data_len = struct.unpack(">I", hdr[83:87])[0]
        rsrc_len = struct.unpack(">I", hdr[87:91])[0]
        data_pad = (data_len + 127) & ~127
        f.seek(128 + data_pad)
        return f.read(rsrc_len)


def parse_resource_fork(fork):
    """Return {(type, id): bytes} from a classic resource fork."""
    data_off, map_off = struct.unpack(">II", fork[0:8])
    type_list_off = map_off + struct.unpack(">H", fork[map_off + 24:map_off + 26])[0]
    ntypes = struct.unpack(">h", fork[type_list_off:type_list_off + 2])[0] + 1
    res = {}
    for t in range(ntypes):
        entry = type_list_off + 2 + t * 8
        rtype = fork[entry:entry + 4].decode("latin-1")
        count = struct.unpack(">h", fork[entry + 4:entry + 6])[0] + 1
        ref_off = type_list_off + struct.unpack(">H", fork[entry + 6:entry + 8])[0]
        for i in range(count):
            ref = ref_off + i * 12
            rid = struct.unpack(">h", fork[ref:ref + 2])[0]
            dloc = struct.unpack(">I", b"\0" + fork[ref + 5:ref + 8])[0]
            dstart = data_off + dloc
            dlen = struct.unpack(">I", fork[dstart:dstart + 4])[0]
            res[(rtype, rid)] = fork[dstart + 4:dstart + 4 + dlen]
    return res


# ---------------------------------------------------------------- PICT decode

def unpackbits(data, i, expected):
    out = bytearray()
    while len(out) < expected:
        n = data[i]
        i += 1
        if n > 128:
            out += bytes([data[i]]) * (257 - n)
            i += 1
        elif n < 128:
            out += data[i:i + 1 + n]
            i += 1 + n
        # n == 128: no-op
    return bytes(out), i


class PictError(Exception):
    pass


def decode_pict(data, name=""):
    """Decode a v1/v2 PICT with BitsRect/PackBitsRect/PackBitsRgn opcodes.

    Returns (w, h, bytes) where each byte is a palette index 0-15 for 4-bit
    sources or 0/1 for 1-bit sources.
    """
    ft, fl, fb, fr = struct.unpack(">hhhh", data[2:10])
    W, H = fr - fl, fb - ft
    canvas = bytearray([0]) * (W * H)
    depth_seen = 1

    v2 = data[10:12] == b"\x00\x11"
    i = 10

    def rd16():
        nonlocal i
        v = struct.unpack(">H", data[i:i + 2])[0]
        i += 2
        return v

    def rd_op():
        nonlocal i
        if v2:
            if i & 1:
                i += 1
            return rd16()
        op = data[i]
        i += 1
        return op

    def skip_region():
        nonlocal i
        sz = struct.unpack(">H", data[i:i + 2])[0]
        i += sz

    while i < len(data):
        op = rd_op()
        if op == 0x00:                       # NOP
            continue
        if op == 0x11:                       # version
            i += 1 if not v2 else 2
            continue
        if op == 0x1E:                       # DefHilite
            continue
        if op == 0x0C00:                     # v2 header
            i += 24
            continue
        if op == 0x01:                       # ClipRgn
            skip_region()
            continue
        if op == 0xA0:                       # short comment
            i += 2
            continue
        if op == 0xA1:                       # long comment
            i += 2
            sz = rd16()
            i += sz + (sz & 1 if v2 else 0)
            continue
        if op == 0xFF:                       # end
            break
        if op in (0x90, 0x91, 0x98, 0x99):
            rowbytes_raw = rd16()
            is_pixmap = bool(rowbytes_raw & 0x8000)
            rowbytes = rowbytes_raw & 0x3FFF
            bt, bl, bb, br = struct.unpack(">hhhh", data[i:i + 8])
            i += 8
            pixel_size = 1
            clut = None
            if is_pixmap:
                i += 2 + 2 + 4 + 4 + 4 + 2   # pmVersion..pixelType
                pixel_size = rd16()
                i += 2 + 2 + 4 + 4 + 4       # cmpCount..pmReserved
                i += 4 + 2                    # ctSeed, ctFlags
                ct_size = rd16()
                clut = []
                for _ in range(ct_size + 1):
                    _v, r, g, b = struct.unpack(">HHHH", data[i:i + 8])
                    i += 8
                    clut.append((r >> 8, g >> 8, b >> 8))
            st, sl, sb, sr = struct.unpack(">hhhh", data[i:i + 8])   # srcRect
            i += 8
            dt, dl, db, dr = struct.unpack(">hhhh", data[i:i + 8])   # dstRect
            i += 8
            i += 2                            # mode
            if op in (0x91, 0x99):
                skip_region()
            rows = []
            band_h = bb - bt
            for _y in range(band_h):
                if op in (0x90, 0x91) or rowbytes < 8:
                    rows.append(data[i:i + rowbytes])
                    i += rowbytes
                else:
                    if rowbytes > 250:
                        bc = rd16()
                    else:
                        bc = data[i]
                        i += 1
                    row, _end = unpackbits(data, i, rowbytes)
                    i += bc
                    rows.append(row)
            depth_seen = max(depth_seen, pixel_size)
            # place band: src->dst; bounds define the band's local space
            for y in range(sb - st):
                dy = dt - ft + y
                if not (0 <= dy < H):
                    continue
                row = rows[(st - bt) + y]
                for x in range(sr - sl):
                    dx = dl - fl + x
                    if not (0 <= dx < W):
                        continue
                    sx = (sl - bl) + x
                    if pixel_size == 1:
                        px = (row[sx >> 3] >> (7 - (sx & 7))) & 1
                    elif pixel_size == 2:
                        px = (row[sx >> 2] >> (6 - 2 * (sx & 3))) & 3
                    elif pixel_size == 4:
                        px = (row[sx >> 1] >> (4 if (sx & 1) == 0 else 0)) & 0xF
                    elif pixel_size == 8:
                        px = row[sx]
                    else:
                        raise PictError(f"{name}: pixelSize {pixel_size}")
                    canvas[dy * W + dx] = px
            continue
        raise PictError(f"{name}: unhandled opcode 0x{op:04X} at offset {i}")
    return W, H, bytes(canvas), depth_seen


# ---------------------------------------------------------------- PNG preview

def png_write(path, w, h, rgb_rows):
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    raw = b"".join(b"\x00" + r for r in rgb_rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def wav_write(path, pcm, rate=22254):
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate, 1, 8))
        f.write(b"data" + struct.pack("<I", len(pcm)) + pcm)


# ---------------------------------------------------------------- main

def main():
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    rez = parse_rez(REZ_PATH)
    snd_fork = parse_resource_fork(macbinary_resource_fork(SOUNDS_PATH))

    # palette from clut 128: 16 entries {value, r, g, b} each 16-bit
    clut_raw = rez[("clut", 128)]
    n_entries = struct.unpack(">h", clut_raw[6:8])[0] + 1
    palette = []
    for k in range(n_entries):
        off = 8 + k * 8
        _v, r, g, b = struct.unpack(">HHHH", clut_raw[off:off + 8])
        palette.append((r >> 8, g >> 8, b >> 8))
    assert len(palette) == 16, f"expected 16-entry clut, got {len(palette)}"

    entries = []  # (fourcc, id, payload, w, h, flags)

    clut_payload = b"".join(struct.pack("<BBBB", r, g, b, 255) for (r, g, b) in palette)
    entries.append(("CLUT", 128, clut_payload, 0, 0, 0))

    # ---- images
    pict_ids = sorted(rid for (t, rid) in rez if t == "PICT")
    for rid in pict_ids:
        name = f"PICT {rid}"
        try:
            w, h, pix, depth = decode_pict(rez[("PICT", rid)], name)
        except PictError as e:
            print(f"  {name}: SKIPPED ({e})")
            continue
        is_mask = rid in MASK_PICTS
        if depth == 1 and not is_mask:
            pix = bytes(15 if p else 0 for p in pix)   # black/white -> palette idx
        entries.append(("PIX ", rid, pix, w, h, 1 if is_mask else 0))
        if is_mask:
            rows = [bytes(255 if pix[y * w + x] else 0 for x in range(w)) * 1 for y in range(h)]
            rgb_rows = [bytes(v for x in range(w) for v in (row[x], row[x], row[x])) for row in [r for r in rows]]
        else:
            rgb_rows = [bytes(v for x in range(w) for v in palette[pix[y * w + x]]) for y in range(h)]
        png_write(os.path.join(PREVIEW_DIR, f"pict_{rid}.png"), w, h, rgb_rows)
        print(f"  {name}: {w}x{h} depth={depth}{' (mask)' if is_mask else ''}")

    # ---- sounds (app 1-25 from .r, external 26-28/100-105 from Para Sounds)
    smsd = {rid: rez[(t, rid)] for (t, rid) in rez if t == "SMSD"}
    for (t, rid), payload in snd_fork.items():
        if t == "SMSD":
            smsd[rid] = payload
    for rid in sorted(smsd):
        raw = smsd[rid]
        priority, loop_flag = struct.unpack(">HH", raw[0:4])
        pcm = raw[8:]
        payload = struct.pack("<HH", priority, loop_flag) + pcm
        entries.append(("SND ", rid, payload, 0, 0, 0))
        wav_write(os.path.join(PREVIEW_DIR, f"snd_{rid}.wav"), pcm)
    print(f"  sounds: {len(smsd)} SMSD clips")

    # ---- physics tables + stars (raw big-endian; loader swaps)
    for t, fourcc in (("forc", "FORC"), ("vert", "VERT"), ("sPts", "SPTS")):
        for (tt, rid) in sorted(k for k in rez if k[0] == t):
            entries.append((fourcc, rid, rez[(tt, rid)], 0, 0, 0))

    # ---- string lists
    for (t, rid) in sorted(k for k in rez if k[0] == "STR#"):
        entries.append(("STR#", rid, rez[(t, rid)], 0, 0, 0))

    # ---- write pack
    toc_size = 4 + 4 + 4 + len(entries) * 28
    off = toc_size
    toc = b"PAR2" + struct.pack("<II", 1, len(entries))
    blob = b""
    for fourcc, rid, payload, w, h, flags in entries:
        toc += struct.pack("<4siIIIII",
                           fourcc.encode("ascii"), rid, off, len(payload), w, h, flags)
        blob += payload
        off += len(payload)
    out_path = os.path.join(OUT_DIR, "pararena2.dat")
    with open(out_path, "wb") as f:
        f.write(toc + blob)
    print(f"wrote {out_path}: {len(entries)} entries, {off} bytes")


if __name__ == "__main__":
    main()
