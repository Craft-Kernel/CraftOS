#!/usr/bin/env python3
"""convert a png into a cimg, the image format craftos loads.

    python tools/img2cimg.py in.png out.cimg --size 640x480 --fit cover --format rgb
    python tools/img2cimg.py logo.png logo.cimg --size 32x32 --trim

only the python standard library is used, so this decodes the png itself.
it handles the common kind: 8 bits per channel, rgb or rgba, not
interlaced. big images take a little while (a 4k png is about a minute).

the cimg format, all little-endian:

    "CIMG"          magic
    u16 width
    u16 height
    u8  encoding    0 = run-length argb, 1 = raw rgb (opaque)
    u8  reserved
    then the pixels, row by row:
      encoding 0: runs of (u8 count, u32 0xaarrggbb), count 1-255
      encoding 1: 3 bytes per pixel, b g r

run-length suits icons (flat areas, transparency). raw rgb suits photos
and gradients, where almost no two neighbouring pixels match.
"""

import argparse
import struct
import sys
import zlib


def decode_png(path):
    """returns (width, height, rows), each row a bytearray of rgba."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{path}: not a png")

    pos, idat = 8, bytearray()
    width = height = depth = ctype = interlace = None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break

    if depth != 8 or ctype not in (2, 6) or interlace:
        sys.exit(f"{path}: only 8-bit, non-interlaced rgb/rgba pngs are supported")

    bpp = 4 if ctype == 6 else 3
    stride = width * bpp
    raw = zlib.decompress(bytes(idat))

    # undo the per-row filters. each row starts with its filter type.
    rows, prev = [], bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        cur = bytearray(raw[start + 1:start + 1 + stride])
        if kind == 1:
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 0xFF
        elif kind == 2:
            for i in range(stride):
                cur[i] = (cur[i] + prev[i]) & 0xFF
        elif kind == 3:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif kind == 4:
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[i] = (cur[i] + pred) & 0xFF
        rows.append(cur)
        prev = cur

    if bpp == 3:
        rgba = []
        for row in rows:
            out = bytearray(width * 4)
            out[0::4], out[1::4], out[2::4] = row[0::3], row[1::3], row[2::3]
            out[3::4] = b"\xff" * width
            rgba.append(out)
        rows = rgba
    return width, height, rows


def trim(width, height, rows):
    """crops to the visible (non-transparent) part, then pads it back to a square."""
    xs, ys = [], []
    for y, row in enumerate(rows):
        alpha = row[3::4]
        hits = [x for x in range(width) if alpha[x] > 8]
        if hits:
            ys.append(y)
            xs += [hits[0], hits[-1]]
    if not xs:
        return width, height, rows
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    side = max(x1 - x0, y1 - y0) + 1
    side += side // 16
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    ox, oy = cx - side // 2, cy - side // 2

    out = []
    for y in range(side):
        row = bytearray(side * 4)
        sy = oy + y
        if 0 <= sy < height:
            for x in range(side):
                sx = ox + x
                if 0 <= sx < width:
                    row[x * 4:x * 4 + 4] = rows[sy][sx * 4:sx * 4 + 4]
        out.append(row)
    return side, side, out


def resize(width, height, rows, out_w, out_h, fit):
    """
    box-filtered resize. cover fills the whole output and crops the
    overflow; contain fits inside it and leaves transparent margins.
    colors are averaged weighted by alpha, so edges don't go dark.
    """
    sx, sy = out_w / width, out_h / height
    scale = max(sx, sy) if fit == "cover" else min(sx, sy)
    vis_w, vis_h = width * scale, height * scale
    off_x, off_y = (out_w - vis_w) / 2, (out_h - vis_h) / 2

    samples = max(1, min(6, int(1 / scale + 0.999)))
    out = []
    for oy in range(out_h):
        row = bytearray(out_w * 4)
        for ox in range(out_w):
            r = g = b = a = 0
            n = 0
            for jy in range(samples):
                py = ((oy + (jy + 0.5) / samples) - off_y) / scale
                if not 0 <= py < height:
                    n += samples
                    continue
                src = rows[int(py)]
                for jx in range(samples):
                    n += 1
                    px = ((ox + (jx + 0.5) / samples) - off_x) / scale
                    if not 0 <= px < width:
                        continue
                    i = int(px) * 4
                    pa = src[i + 3]
                    r += src[i] * pa
                    g += src[i + 1] * pa
                    b += src[i + 2] * pa
                    a += pa
            if a:
                row[ox * 4:ox * 4 + 4] = bytes((r // a, g // a, b // a, a // n))
        out.append(row)
    return out


def encode(width, height, rows, encoding):
    out = bytearray(b"CIMG" + struct.pack("<HHBB", width, height, encoding, 0))
    if encoding == 1:
        for row in rows:
            px = bytearray(width * 3)
            px[0::3], px[1::3], px[2::3] = row[2::4], row[1::4], row[0::4]
            out += px
        return out

    def argb(row, x):
        r, g, b, a = row[x * 4:x * 4 + 4]
        return a << 24 | r << 16 | g << 8 | b

    run_px, run_len = None, 0
    for row in rows:
        for x in range(width):
            p = argb(row, x)
            if p == run_px and run_len < 255:
                run_len += 1
                continue
            if run_len:
                out += struct.pack("<BI", run_len, run_px)
            run_px, run_len = p, 1
    if run_len:
        out += struct.pack("<BI", run_len, run_px)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("--size", required=True, help="WIDTHxHEIGHT")
    p.add_argument("--fit", choices=["cover", "contain"], default="contain")
    p.add_argument("--format", choices=["rle", "rgb"], default="rle")
    p.add_argument("--trim", action="store_true", help="crop away transparent margins first")
    args = p.parse_args()

    out_w, out_h = (int(v) for v in args.size.lower().split("x"))
    width, height, rows = decode_png(args.input)
    if args.trim:
        width, height, rows = trim(width, height, rows)
    rows = resize(width, height, rows, out_w, out_h, args.fit)
    data = encode(out_w, out_h, rows, 1 if args.format == "rgb" else 0)
    open(args.output, "wb").write(data)
    print(f"{args.output}: {out_w}x{out_h}, {len(data)} bytes")


if __name__ == "__main__":
    main()
