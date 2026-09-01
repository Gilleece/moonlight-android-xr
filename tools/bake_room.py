#!/usr/bin/env python3
#
# Bakes a glTF binary (.glb) room model into the flat mesh file the renderer
# loads from assets. The renderer has no glTF loader on purpose: this script
# does the parsing once, here, and ships only positions, normals and texture
# coordinates.
#
#   python3 tools/bake_room.py tools/rooms/psx-cinema/cinema.glb \
#       app/src/main/assets/rooms/psx_cinema.room
#
# Output layout, little endian:
#   char[4]  magic 'MXR1'
#   uint32   vertex count
#   uint32   index count
#   float[8] per vertex: position xyz, normal xyz, uv
#   uint16   indices
#
# Only the simplest models are accepted: one mesh, one primitive, triangles,
# 16 bit indices, no node transform. Anything richer should be flattened in a
# 3d tool first rather than taught to this script.

import json
import struct
import sys


def accessor(gltf, blob, index):
    a = gltf["accessors"][index]
    view = gltf["bufferViews"][a["bufferView"]]
    start = view.get("byteOffset", 0) + a.get("byteOffset", 0)
    comps = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[a["type"]]
    fmt, size = {5123: ("H", 2), 5126: ("f", 4)}[a["componentType"]]
    stride = view.get("byteStride", comps * size)
    out = []
    for i in range(a["count"]):
        base = start + i * stride
        out.append(struct.unpack_from("<" + fmt * comps, blob, base))
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: bake_room.py model.glb out.room")

    data = open(sys.argv[1], "rb").read()
    magic, version, _ = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67 or version != 2:
        sys.exit("not a glb v2 file")

    json_len, _ = struct.unpack_from("<II", data, 12)
    gltf = json.loads(data[20:20 + json_len])
    bin_off = 20 + json_len
    bin_len, _ = struct.unpack_from("<II", data, bin_off)
    blob = data[bin_off + 8:bin_off + 8 + bin_len]

    meshes = gltf.get("meshes", [])
    if len(meshes) != 1 or len(meshes[0]["primitives"]) != 1:
        sys.exit("expected exactly one mesh with one primitive")
    node = gltf["nodes"][gltf["scenes"][0]["nodes"][0]]
    if any(k in node for k in ("translation", "rotation", "scale", "matrix")):
        sys.exit("node carries a transform, flatten the model first")

    prim = meshes[0]["primitives"][0]
    if prim.get("mode", 4) != 4:
        sys.exit("primitive is not triangles")

    indices = [i[0] for i in accessor(gltf, blob, prim["indices"])]
    pos = accessor(gltf, blob, prim["attributes"]["POSITION"])
    normal = accessor(gltf, blob, prim["attributes"]["NORMAL"])
    uv = accessor(gltf, blob, prim["attributes"]["TEXCOORD_0"])

    if len(pos) > 0xFFFF:
        sys.exit("more vertices than 16 bit indices can address")
    if max(indices) >= len(pos):
        sys.exit("index out of range")

    with open(sys.argv[2], "wb") as out:
        out.write(b"MXR1")
        out.write(struct.pack("<II", len(pos), len(indices)))
        for p, n, t in zip(pos, normal, uv):
            out.write(struct.pack("<8f", *p, *n, *t))
        for i in indices:
            out.write(struct.pack("<H", i))

    tris = len(indices) // 3
    print(f"{sys.argv[2]}: {len(pos)} vertices, {tris} triangles")


if __name__ == "__main__":
    main()
