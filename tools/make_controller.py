#!/usr/bin/env python3
#
# Writes the generic VR controller the app draws at each hand's grip, as a
# glTF binary for tools/bake_room.py to bake. Plain Python, nothing to install:
# a handle, a head with a thumbstick on it, a trigger, and a ring over the top,
# painted in vertex colours with no texture.
#
#   python3 tools/make_controller.py controller.glb
#   python3 tools/bake_room.py controller.glb app/src/main/assets/models/controller.room
#
# The model is a right hand controller in OpenXR's grip space, in metres: the
# origin is the middle of the handle where the palm closes round it, +x points
# to the right, +y up, and -z forward along a straightened index finger. The
# renderer mirrors it in x for the left hand.

import json
import math
import struct
import sys

# Linear colours, which glTF keeps them in. The bake turns them into the sRGB
# the renderer draws with: a neutral dark grey body, a lighter ring.
BODY = (0.084, 0.084, 0.088)
TRIGGER = (0.052, 0.052, 0.055)
STICK = (0.020, 0.020, 0.021)
RING = (0.56, 0.56, 0.58)


class Mesh:
    def __init__(self):
        self.pos = []
        self.normal = []
        self.color = []
        self.indices = []

    def vertex(self, p, n, color):
        self.pos.append(p)
        self.normal.append(normalize(n))
        self.color.append(color)
        return len(self.pos) - 1

    def grid(self, rows, cols, at, color):
        # A band of rows by cols vertices from at(i, j) -> (point, normal),
        # stitched into triangles, each row closing round on itself
        base = len(self.pos)
        for i in range(rows):
            for j in range(cols):
                p, n = at(i, j)
                self.vertex(p, n, color)
        for i in range(rows - 1):
            for j in range(cols):
                a = base + i * cols + j
                b = base + i * cols + (j + 1) % cols
                c = a + cols
                d = b + cols
                self.indices += [a, c, b, b, c, d]

    def fan(self, centre, normal, ring, color):
        # A flat cap over a loop of points, from its centre
        mid = self.vertex(centre, normal, color)
        first = len(self.pos)
        for p in ring:
            self.vertex(p, normal, color)
        count = len(ring)
        for j in range(count):
            self.indices += [mid, first + j, first + (j + 1) % count]


def normalize(v):
    length = math.sqrt(sum(c * c for c in v))
    return tuple(c / length for c in v) if length > 1e-12 else (0.0, 1.0, 0.0)


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def scale(v, s):
    return tuple(x * s for x in v)


def rot_x(v, angle):
    c, s = math.cos(angle), math.sin(angle)
    return (v[0], v[1] * c - v[2] * s, v[1] * s + v[2] * c)


def rot_z(v, angle):
    c, s = math.cos(angle), math.sin(angle)
    return (v[0] * c - v[1] * s, v[0] * s + v[1] * c, v[2])


def ellipsoid(mesh, centre, radii, tilt, color, rings, segments):
    # Latitude rows from pole to pole, tipped about x by tilt
    def at(i, j):
        theta = math.pi * i / (rings - 1)
        phi = 2.0 * math.pi * j / segments
        unit = (math.sin(theta) * math.cos(phi), math.cos(theta),
                math.sin(theta) * math.sin(phi))
        p = (unit[0] * radii[0], unit[1] * radii[1], unit[2] * radii[2])
        n = (unit[0] / radii[0], unit[1] / radii[1], unit[2] / radii[2])
        return add(rot_x(p, tilt), centre), rot_x(n, tilt)
    mesh.grid(rings, segments, at, color)


def handle(mesh, bottom, top, profile, color, segments):
    # A tube along bottom to top, its radius at each step of the profile, with
    # a shallow cone for a foot. The normals lean with the taper.
    axis = tuple(t - b for t, b in zip(top, bottom))
    length = math.sqrt(sum(c * c for c in axis))
    up = normalize(axis)
    side = (1.0, 0.0, 0.0)
    front = normalize(cross(up, side))
    steps = len(profile)

    def at(i, j):
        f = i / (steps - 1)
        phi = 2.0 * math.pi * j / segments
        r = profile[i]
        slope = (profile[min(i + 1, steps - 1)] - profile[max(i - 1, 0)]) \
            / (length * (min(i + 1, steps - 1) - max(i - 1, 0)) / (steps - 1))
        out = add(scale(side, math.cos(phi)), scale(front, math.sin(phi)))
        p = add(add(bottom, scale(axis, f)), scale(out, r))
        return p, add(out, scale(up, -slope))
    mesh.grid(steps, segments, at, color)

    # The foot, a cone just proud of the first ring
    r = profile[0]
    loop = []
    for j in range(segments):
        phi = 2.0 * math.pi * j / segments
        out = add(scale(side, math.cos(phi)), scale(front, math.sin(phi)))
        loop.append(add(bottom, scale(out, r)))
    mesh.fan(add(bottom, scale(up, -0.004)), scale(up, -1.0), list(reversed(loop)), color)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def torus(mesh, centre, major, minor, tilt_x, tilt_z, color, segments, sides):
    # The ring, lying in its own xz plane before it is tipped
    def at(i, j):
        u = 2.0 * math.pi * i / segments
        v = 2.0 * math.pi * j / sides
        across = (math.cos(u), 0.0, math.sin(u))
        n = (across[0] * math.cos(v), math.sin(v), across[2] * math.cos(v))
        p = add(scale(across, major), scale(n, minor))
        p = rot_z(rot_x(p, tilt_x), tilt_z)
        n = rot_z(rot_x(n, tilt_x), tilt_z)
        return add(p, centre), n
    mesh.grid(segments + 1, sides, at, color)


def stick(mesh, centre, radius, height, tilt, color, segments):
    # A short post with a flat cap, tipped with the head it stands on
    def at(i, j):
        phi = 2.0 * math.pi * j / segments
        out = (math.cos(phi), 0.0, math.sin(phi))
        p = add(scale(out, radius), (0.0, height * i, 0.0))
        return add(rot_x(p, tilt), centre), rot_x(out, tilt)
    mesh.grid(2, segments, at, color)
    loop = []
    for j in range(segments):
        phi = 2.0 * math.pi * j / segments
        p = (radius * math.cos(phi), height, radius * math.sin(phi))
        loop.append(add(rot_x(p, tilt), centre))
    mesh.fan(add(rot_x((0.0, height, 0.0), tilt), centre), rot_x((0.0, 1.0, 0.0), tilt),
             list(reversed(loop)), color)


def build():
    mesh = Mesh()
    # The handle leans forward at the top, the way a pistol grip does
    handle(mesh, (0.0, -0.055, 0.022), (0.0, 0.026, -0.012),
           [0.0135, 0.0155, 0.0165, 0.0170, 0.0172, 0.0168], BODY, 20)
    # The head over it, the face the thumb rests on tipped down at the front
    head_tilt = math.radians(-14.0)
    ellipsoid(mesh, (0.0, 0.036, -0.022), (0.026, 0.0145, 0.036), head_tilt, BODY, 12, 24)
    # The thumbstick toward the inner edge of the face
    stick(mesh, (-0.008, 0.047, -0.016), 0.0085, 0.007, head_tilt, STICK, 12)
    # The trigger under the front of the head, swept back at its foot
    ellipsoid(mesh, (0.0, 0.010, -0.052), (0.0075, 0.0165, 0.0085), math.radians(32.0),
              TRIGGER, 8, 12)
    # The ring stands over the head, its foot sunk into it, upright along the
    # controller and leaning out past the back of the hand at the top
    torus(mesh, (0.006, 0.056, -0.024), 0.037, 0.0055, 0.0, math.radians(-100.0),
          RING, 40, 8)
    return mesh


def accessor_bytes(values, fmt):
    return b"".join(struct.pack("<" + fmt, *v) if isinstance(v, tuple)
                    else struct.pack("<" + fmt, v) for v in values)


def write_glb(mesh, path):
    pos = accessor_bytes(mesh.pos, "3f")
    normal = accessor_bytes(mesh.normal, "3f")
    color = accessor_bytes(mesh.color, "3f")
    index = accessor_bytes(mesh.indices, "I")
    views = []
    blob = b""
    for chunk in (pos, normal, color, index):
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(chunk)})
        blob += chunk
    lows = [min(p[k] for p in mesh.pos) for k in range(3)]
    highs = [max(p[k] for p in mesh.pos) for k in range(3)]
    count = len(mesh.pos)
    gltf = {
        "asset": {"version": "2.0", "generator": "tools/make_controller.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "Controller", "mesh": 0}],
        "meshes": [{"name": "Controller", "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "COLOR_0": 2},
            "indices": 3, "mode": 4}]}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": count, "type": "VEC3",
             "min": lows, "max": highs},
            {"bufferView": 1, "componentType": 5126, "count": count, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": count, "type": "VEC3"},
            {"bufferView": 3, "componentType": 5125, "count": len(mesh.indices),
             "type": "SCALAR"},
        ],
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob)}],
    }
    text = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    text += b" " * (-len(text) % 4)
    blob += b"\0" * (-len(blob) % 4)
    total = 12 + 8 + len(text) + 8 + len(blob)
    with open(path, "wb") as out:
        out.write(struct.pack("<III", 0x46546C67, 2, total))
        out.write(struct.pack("<II", len(text), 0x4E4F534A))
        out.write(text)
        out.write(struct.pack("<II", len(blob), 0x004E4942))
        out.write(blob)
    print(f"{path}: {count} vertices, {len(mesh.indices) // 3} triangles, {total} bytes")


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: make_controller.py out.glb")
    write_glb(build(), sys.argv[1])


if __name__ == "__main__":
    main()
