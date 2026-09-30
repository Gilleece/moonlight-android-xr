#!/usr/bin/env python3
#
# Compresses a room's texture atlas into the .atlas file the renderer uploads
# as it is: ASTC blocks for every mip level, baked here so the headset neither
# decodes a PNG nor builds a mip chain. The encoding is done by astcenc
# (https://github.com/ARM-software/astc-encoder, Apache-2.0), a build tool
# only: nothing of it ships. It is found through $ASTCENC, or on the PATH.
#
#   python3 tools/atlas_astc.py atlas.png --size 4096 \
#       --out app/src/main/assets/rooms/home_theater_0.atlas
#   python3 tools/atlas_astc.py atlas.png --size 2048 \
#       --out app/src/main/assets/rooms/home_theater_lo_0.atlas
#
# Every level is a Lanczos resize of the source itself rather than of the level
# above it, encoded with the LDR profile so the colours come out as an RGBA8
# upload of the PNG would.
#
# Output layout, little endian:
#   char[4]  magic 'MXA1'
#   uint32   width of level 0
#   uint32   height of level 0
#   uint32   block width
#   uint32   block height
#   uint32   level count
#   per level, largest first: uint32 byte length, then that many bytes of
#            ASTC blocks, 16 bytes a block, rows from the top of the picture
#
# Level i is max(1, width >> i) by max(1, height >> i), the size GL expects of
# a mip level, and the chain runs down to 1x1. A level's length is
# ceil(w / block width) * ceil(h / block height) * 16, with astcenc's own file
# header stripped. xr_atlas.c reads the same layout.
#
# python3 tools/atlas_astc.py --check runs a self check on generated images.

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

from PIL import Image

MAGIC = b"MXA1"
HEADER = struct.Struct("<4sIIIII")
ASTC_MAGIC = 0x5CA1AB13
BLOCKS = ("4x4", "6x6", "8x8")
QUALITIES = ("fast", "medium", "thorough", "exhaustive")
# The names upstream's build gives the binary, plain first
ASTCENC_NAMES = ("astcenc", "astcenc-neon", "astcenc-avx2", "astcenc-sse4.1")


def find_astcenc():
    candidates = [os.environ.get("ASTCENC")] + [shutil.which(n) for n in ASTCENC_NAMES]
    for path in candidates:
        if path and os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    sys.exit("astcenc not found: set ASTCENC to the binary or put it on the PATH")


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def level_sizes(width, height):
    sizes = [(width, height)]
    while sizes[-1] != (1, 1):
        w, h = sizes[-1]
        sizes.append((max(1, w // 2), max(1, h // 2)))
    return sizes


def level_bytes(width, height, bw, bh):
    return -(-width // bw) * -(-height // bh) * 16


def parse_block(block):
    bw, bh = (int(v) for v in block.split("x"))
    return bw, bh


# Scaled so the longer side is size, never up
def target_size(source, size):
    w, h = source.size
    if size is None:
        return w, h
    longer = max(w, h)
    if size > longer:
        sys.exit(f"--size {size} is larger than the source's {w}x{h}")
    return max(1, round(w * size / longer)), max(1, round(h * size / longer))


# One level through astcenc, with its header checked and cut off
def encode_level(astcenc, image, block, quality, work, index):
    png = os.path.join(work, f"level{index}.png")
    out = os.path.join(work, f"level{index}.astc")
    image.save(png)
    subprocess.run([astcenc, "-cl", png, out, block, "-" + quality, "-silent"],
                   check=True)
    data = read_bytes(out)
    magic, bx, by, bz = struct.unpack_from("<IBBB", data, 0)
    dims = [int.from_bytes(data[7 + 3 * i:10 + 3 * i], "little") for i in range(3)]
    bw, bh = parse_block(block)
    if magic != ASTC_MAGIC or (bx, by, bz) != (bw, bh, 1) or dims != [*image.size, 1]:
        sys.exit(f"astcenc wrote an unexpected header for level {index}")
    blocks = data[16:]
    if len(blocks) != level_bytes(*image.size, bw, bh):
        sys.exit(f"level {index} is {len(blocks)} bytes, expected "
                 f"{level_bytes(*image.size, bw, bh)}")
    return blocks


def convert(source_path, out_path, size=None, block="6x6", quality="thorough",
            astcenc=None):
    if block not in BLOCKS:
        sys.exit(f"block {block} is not one of {', '.join(BLOCKS)}")
    if quality not in QUALITIES:
        sys.exit(f"quality {quality} is not one of {', '.join(QUALITIES)}")
    astcenc = astcenc or find_astcenc()
    source = Image.open(source_path)
    source.load()
    if source.mode not in ("RGB", "RGBA"):
        source = source.convert("RGBA" if "A" in source.getbands() else "RGB")
    width, height = target_size(source, size)
    bw, bh = parse_block(block)
    levels = []
    with tempfile.TemporaryDirectory() as work:
        for index, dims in enumerate(level_sizes(width, height)):
            image = source if dims == source.size else source.resize(
                    dims, Image.Resampling.LANCZOS)
            levels.append(encode_level(astcenc, image, block, quality, work, index))
    with open(out_path, "wb") as out:
        out.write(HEADER.pack(MAGIC, width, height, bw, bh, len(levels)))
        for data in levels:
            out.write(struct.pack("<I", len(data)))
            out.write(data)
    return width, height, len(levels), os.path.getsize(out_path)


# The header and every level, checked against the layout above
def read_atlas(path):
    data = read_bytes(path)
    if len(data) < HEADER.size:
        raise ValueError("shorter than the header")
    magic, width, height, bw, bh, count = HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        raise ValueError(f"magic {magic!r}")
    sizes = level_sizes(width, height)
    if count != len(sizes):
        raise ValueError(f"{count} levels, a full chain is {len(sizes)}")
    at = HEADER.size
    levels = []
    for w, h in sizes:
        (length,) = struct.unpack_from("<I", data, at)
        if length != level_bytes(w, h, bw, bh):
            raise ValueError(f"level {w}x{h} is {length} bytes")
        at += 4
        levels.append((w, h, data[at:at + length]))
        at += length
    if at != len(data):
        raise ValueError(f"{len(data) - at} bytes past the last level")
    return width, height, bw, bh, levels


def self_check():
    with tempfile.TemporaryDirectory() as work:
        for (w, h), block, size in (((64, 64), "6x6", None), ((37, 19), "4x4", None),
                                    ((128, 64), "8x8", 50)):
            src = os.path.join(work, "gradient.png")
            image = Image.new("RGB", (w, h))
            image.putdata([(x * 255 // w, y * 255 // h, (x + y) % 256)
                           for y in range(h) for x in range(w)])
            image.save(src)
            out = os.path.join(work, "gradient.atlas")
            width, height, count, _ = convert(src, out, size=size, block=block,
                                              quality="medium")
            first = read_bytes(out)
            convert(src, out, size=size, block=block, quality="medium")
            if read_bytes(out) != first:
                sys.exit(f"{w}x{h} {block}: two runs differ")
            rw, rh, bw, bh, levels = read_atlas(out)
            expect = level_sizes(width, height)
            if (rw, rh) != (width, height) or [(lw, lh) for lw, lh, _ in levels] != expect:
                sys.exit(f"{w}x{h} {block}: levels read back wrong")
            if f"{bw}x{bh}" != block:
                sys.exit(f"{w}x{h} {block}: block read back as {bw}x{bh}")
            print(f"{w}x{h} -> {width}x{height} {block}: {count} levels, down to "
                  f"{levels[-1][0]}x{levels[-1][1]}, ok")
    print("self check passed")


def main():
    parser = argparse.ArgumentParser(description="PNG atlas to a mip chained ASTC .atlas")
    parser.add_argument("png", nargs="?")
    parser.add_argument("--out", help="the .atlas to write, beside the PNG by default")
    parser.add_argument("--size", type=int,
                        help="resize first so the longer side is this many pixels")
    parser.add_argument("--block", default="6x6", choices=BLOCKS)
    parser.add_argument("--quality", default="thorough", choices=QUALITIES)
    parser.add_argument("--check", action="store_true", help="run the self check")
    args = parser.parse_args()
    if args.check:
        self_check()
        return
    if not args.png:
        parser.error("a PNG is needed")
    out = args.out or os.path.splitext(args.png)[0] + ".atlas"
    width, height, count, size = convert(args.png, out, args.size, args.block,
                                         args.quality)
    print(f"{out}: {width}x{height}, {args.block}, {count} levels, {size} bytes")


if __name__ == "__main__":
    main()
