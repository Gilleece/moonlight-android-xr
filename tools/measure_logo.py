#!/usr/bin/env python3
#
# Measures the logo the session splash draws, from the source PNG at the
# repository root, and prints the numbers XrPanels.Logo keeps: how far the six
# wedges reach, half the width of the spokes between them, how far each spoke
# is off its 45 degree line, and the boxes of the X and the R. Every length is
# a fraction of the disc's radius, measured from the disc's centre, with y
# running down the image.
#
#   python3 tools/measure_logo.py [logo.png]
#
# Standard library only, so the PNG is decoded here: 8 bit RGBA, not
# interlaced, which is what the logo is.

import math
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
LOGO = os.path.normpath(os.path.join(HERE, "..", "moonlight-xr-logo-transparent.png"))

# The disc is a slate grey at about 0.35 and the wedges and letters white, so
# lightness is scaled to run 0 to 1 between the two and an edge is where it
# crosses a half
SLATE = 0.36
# The wedges sit on screen angles from straight down, clockwise, 45 degrees
# each, so the boundaries are at 90, 135 and on round to 360
FIRST_DEG = 90
WEDGE_DEG = 45
WEDGES = 6


def read_rgba(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(path + ": not a PNG")
    pos = 8
    width = height = 0
    idat = bytearray()
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or colour != 6 or interlace != 0:
                sys.exit(path + ": wants 8 bit RGBA, not interlaced")
        elif kind == b"IDAT":
            idat += body
    raw = zlib.decompress(bytes(idat))
    stride = width * 4
    rows = []
    prev = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        line = bytearray(raw[start + 1:start + 1 + stride])
        if kind == 1:
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 255
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif kind == 3:
            for i in range(stride):
                left = line[i - 4] if i >= 4 else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif kind == 4:
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 255
        rows.append(line)
        prev = line
    return width, height, rows


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else LOGO
    width, height, rows = read_rgba(path)

    # The disc is everything opaque: its centre is the centroid of the alpha
    # and its radius the one whose area matches
    alpha = [[row[x * 4 + 3] / 255.0 for x in range(width)] for row in rows]
    light = []
    for y in range(height):
        row = rows[y]
        out = []
        for x in range(width):
            if row[x * 4 + 3] < 128:
                out.append(0.0)
                continue
            lum = (row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2]) / 765.0
            out.append(min(1.0, max(0.0, (lum - SLATE) / (1.0 - SLATE))))
        light.append(out)
    area = sx = sy = 0.0
    for y in range(height):
        for x in range(width):
            a = alpha[y][x]
            area += a
            sx += a * (x + 0.5)
            sy += a * (y + 0.5)
    cx, cy = sx / area, sy / area
    radius = math.sqrt(area / math.pi)

    def sample(x, y):
        # Bilinear, in pixel centre coordinates
        x -= 0.5
        y -= 0.5
        x0, y0 = int(math.floor(x)), int(math.floor(y))
        fx, fy = x - x0, y - y0
        if x0 < 0 or y0 < 0 or x0 + 1 >= width or y0 + 1 >= height:
            return 0.0
        return (light[y0][x0] * (1 - fx) * (1 - fy) + light[y0][x0 + 1] * fx * (1 - fy)
                + light[y0 + 1][x0] * (1 - fx) * fy + light[y0 + 1][x0 + 1] * fx * fy)

    def crossing(x0, y0, dx, dy, start, end, step=0.1):
        # Where the lightness first drops through a half walking from start
        # to end along the line, or None
        before = sample(x0 + dx * start, y0 + dy * start)
        t = start
        while t < end:
            t2 = t + step
            after = sample(x0 + dx * t2, y0 + dy * t2)
            if before >= 0.5 > after:
                return t + step * (before - 0.5) / (before - after)
            before, t = after, t2
        return None

    # How far each wedge reaches, along rays well clear of its edges
    reach = []
    for k in range(WEDGES):
        for off in range(10, WEDGE_DEG - 9, 5):
            a = math.radians(FIRST_DEG + WEDGE_DEG * k + off)
            r = crossing(cx, cy, math.cos(a), math.sin(a), 0.5 * radius, 0.98 * radius)
            if r is not None:
                reach.append(r)
    wedge_r = sum(reach) / len(reach)

    # Each spoke's edges, across the boundary at a run of distances out. The
    # offset is from the boundary line, positive toward the wedge beyond it;
    # a straight wedge edge parallel to the line keeps the same offset at
    # every distance, so the change over the run is the angle it is off by.
    print("boundary  wedge before ends  wedge after starts  (px, at 100 / 280 out)")
    halves = []
    worst_deg = 0.0
    for k in range(WEDGES + 1):
        a = math.radians(FIRST_DEG + WEDGE_DEG * k)
        ux, uy = math.cos(a), math.sin(a)
        nx, ny = -uy, ux
        sides = []
        for sign, wanted in ((-1, k > 0), (1, k < WEDGES)):
            if not wanted:
                sides.append(None)
                continue
            offs = []
            for r in (100, 280):
                # Walk from inside the wedge back toward the line
                bx, by = cx + ux * r + nx * sign * 40, cy + uy * r + ny * sign * 40
                s = crossing(bx, by, -nx * sign, -ny * sign, 0, 40)
                offs.append(None if s is None else 40 - s)
            sides.append(offs)
            if offs[0] is not None and offs[1] is not None:
                halves.extend(offs)
                worst_deg = max(worst_deg, abs(math.degrees(math.atan2(offs[1] - offs[0], 180))))
        fmt = lambda o: "-" if o is None else "%.2f / %.2f" % (o[0], o[1])
        print("%5d deg  %17s  %18s" % (FIRST_DEG + WEDGE_DEG * k, fmt(sides[0]), fmt(sides[1])))
    half_spoke = sum(halves) / len(halves)

    # The letters: what is light in the quadrant the wedges leave, inside the
    # disc's edge (whose antialiasing is lighter than the slate), split into
    # the X and the R at the gap between them
    columns = {}
    for y in range(int(cy) + 5, height):
        for x in range(int(cx) + 5, width):
            if light[y][x] >= 0.5 and math.hypot(x + 0.5 - cx, y + 0.5 - cy) < 0.95 * radius:
                lo, hi = columns.get(x, (y, y))
                columns[x] = (min(lo, y), max(hi, y))
    runs = []
    for x in sorted(columns):
        if runs and x == runs[-1][1] + 1:
            runs[-1][1] = x
        else:
            runs.append([x, x])
    if len(runs) != 2:
        sys.exit("expected the X and the R, found %d runs of columns" % len(runs))

    rel = lambda v, c: (v - c) / radius
    print()
    print("disc: centre %.2f, %.2f px, radius %.2f px" % (cx, cy, radius))
    print("WEDGE_R = %.4f (%.1f px)" % (wedge_r / radius, wedge_r))
    print("HALF_SPOKE = %.4f (%.2f px), the worst spoke edge %.2f deg off its line"
          % (half_spoke / radius, half_spoke, worst_deg))
    for name, (x0, x1) in zip(("X_BOX", "R_BOX"), runs):
        top = min(columns[x][0] for x in range(x0, x1 + 1))
        bottom = max(columns[x][1] for x in range(x0, x1 + 1)) + 1
        print("%s = { %.4f, %.4f, %.4f, %.4f } (left, top, right, bottom)"
              % (name, rel(x0, cx), rel(top, cy), rel(x1 + 1, cx), rel(bottom, cy)))


if __name__ == "__main__":
    main()
