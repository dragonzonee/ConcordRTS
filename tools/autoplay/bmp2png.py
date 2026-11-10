"""Convert autoplay screenshots (32-bit BMP) to PNG, optionally scaled down.

  python tools/autoplay/bmp2png.py <file.bmp|folder> [scale]
"""
import os
import struct
import sys
import zlib


def bmp_to_png(src, dst, scale=1.0):
    data = open(src, "rb").read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    assert bpp == 32, "expects 32-bit BMP"
    bottom_up = height > 0
    height = abs(height)
    step = max(1, int(round(1 / scale)))
    out_w, out_h = width // step, height // step
    rows = []
    for oy in range(out_h):
        y = oy * step
        src_row = (height - 1 - y) if bottom_up else y
        base = offset + src_row * width * 4
        row = bytearray(b"\0")
        for ox in range(out_w):
            p = base + ox * step * 4
            b, g, r = data[p], data[p + 1], data[p + 2]
            row += bytes((r, g, b))
        rows.append(bytes(row))

    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", out_w, out_h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)


if __name__ == "__main__":
    target = sys.argv[1]
    scale = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
    files = [os.path.join(target, f) for f in os.listdir(target) if f.endswith(".bmp")] if os.path.isdir(target) else [target]
    for f in files:
        bmp_to_png(f, f[:-4] + ".png", scale)
        print(f[:-4] + ".png")
