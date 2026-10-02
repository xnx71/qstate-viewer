#!/usr/bin/env python3
"""xwd2png.py <in.xwd> <out.png>   (or '-' for stdin) - converts an X11 XWD screenshot (xwd -root) to PNG.

Pure standard library. Supports ZPixmap TrueColor / DirectColor images with 16, 24 or 32 bits per pixel
(what Xvfb produces). Used by scripts/gui-selftest.sh to capture the window of the headless self test.
"""
import struct
import sys
import zlib


def read_xwd(data):
    fields = struct.unpack(">25I", data[:100])
    (header_size, file_version, pixmap_format, pixmap_depth, width, height, xoffset, byte_order,
     bitmap_unit, bitmap_bit_order, bitmap_pad, bits_per_pixel, bytes_per_line, visual_class,
     red_mask, green_mask, blue_mask, bits_per_rgb, colormap_entries, ncolors, *_rest) = fields
    if file_version != 7:
        raise SystemExit("unsupported XWD version %d" % file_version)
    if pixmap_format != 2:
        raise SystemExit("only ZPixmap XWD files are supported")
    if bits_per_pixel not in (16, 24, 32):
        raise SystemExit("unsupported bits_per_pixel %d" % bits_per_pixel)
    offset = header_size + ncolors * 12
    pixels = data[offset:offset + bytes_per_line * height]
    if len(pixels) < bytes_per_line * height:
        raise SystemExit("truncated XWD file")
    return width, height, bits_per_pixel, bytes_per_line, byte_order, (red_mask, green_mask, blue_mask), pixels


def shift_and_bits(mask):
    shift = (mask & -mask).bit_length() - 1 if mask else 0
    bits = bin(mask >> shift).count("1") if mask else 0
    return shift, bits


def to_rgb_rows(width, height, bpp, stride, byte_order, masks, pixels):
    little = byte_order == 0
    bytes_pp = bpp // 8
    chans = [shift_and_bits(m) for m in masks]
    rows = []
    for y in range(height):
        row = bytearray(width * 3)
        base = y * stride
        for x in range(width):
            p = pixels[base + x * bytes_pp: base + (x + 1) * bytes_pp]
            value = int.from_bytes(p, "little" if little else "big")
            for c, (shift, bits) in enumerate(chans):
                v = (value >> shift) & ((1 << bits) - 1)
                row[x * 3 + c] = v * 255 // ((1 << bits) - 1) if bits else 0
        rows.append(bytes(row))
    return rows


def write_png(path, width, height, rows):
    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + row for row in rows)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as out:
        out.write(png)


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__)
    data = sys.stdin.buffer.read() if argv[1] == "-" else open(argv[1], "rb").read()
    width, height, bpp, stride, byte_order, masks, pixels = read_xwd(data)
    write_png(argv[2], width, height, to_rgb_rows(width, height, bpp, stride, byte_order, masks, pixels))


if __name__ == "__main__":
    main(sys.argv)
