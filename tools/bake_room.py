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
# A file that carries more than the room, such as marker empties for the seat
# and the screen, names the mesh to bake by its own name or by the name of the
# node it hangs off. --texture writes the embedded atlas out as the PNG it
# already is, in the same pass:
#
#   python3 tools/bake_room.py --mesh Home-Theater-HighRes_Baked \
#       --texture atlas.png model.glb app/src/main/assets/rooms/home_theater.room
#
# Output layout, little endian:
#   char[4]  magic 'MXR1'
#   uint32   vertex count
#   uint32   index count
#   float[8] per vertex: position xyz, normal xyz, uv
#   uint16   indices
#
# Only the simplest models are accepted: one primitive, triangles, 16 bit
# indices, no transform on the node carrying the mesh. Anything richer should
# be flattened in a 3d tool first rather than taught to this script.

import argparse
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


# The mesh to bake. With no name the file must hold exactly one.
def pick_mesh(gltf, name):
    meshes = gltf.get("meshes", [])
    if name is None:
        if len(meshes) != 1:
            sys.exit(f"{len(meshes)} meshes in the file, name one with --mesh")
        return 0

    found = [i for i, m in enumerate(meshes) if m.get("name") == name]
    for node in gltf.get("nodes", []):
        if node.get("name") == name and "mesh" in node and node["mesh"] not in found:
            found.append(node["mesh"])
    if len(found) != 1:
        sys.exit(f"{name} names {len(found)} meshes, expected one")
    return found[0]


# Only the nodes carrying this mesh have to be free of transforms, since the
# positions go out as they were authored. Marker nodes elsewhere in the file
# may be placed however they like.
def check_nodes(gltf, mesh):
    for node in gltf.get("nodes", []):
        if node.get("mesh") != mesh:
            continue
        if any(k in node for k in ("translation", "rotation", "scale", "matrix")):
            sys.exit(f"node {node.get('name', '?')} carries a transform, flatten the model first")


# The embedded atlas is stored as an encoded PNG, so its bytes are written out
# unchanged
def write_texture(gltf, blob, path):
    images = gltf.get("images", [])
    if len(images) != 1:
        sys.exit(f"{len(images)} embedded images, expected one")
    image = images[0]
    if image.get("mimeType") != "image/png":
        sys.exit(f"the embedded image is {image.get('mimeType')}, not image/png")
    if "bufferView" not in image:
        sys.exit("the image is a file reference rather than embedded")
    view = gltf["bufferViews"][image["bufferView"]]
    start = view.get("byteOffset", 0)
    with open(path, "wb") as out:
        out.write(blob[start:start + view["byteLength"]])
    print(f"{path}: {view['byteLength']} bytes")


def main():
    parser = argparse.ArgumentParser(description="bake a glb room into a .room mesh file")
    parser.add_argument("--mesh", help="the mesh, or the node carrying it, in a file with several")
    parser.add_argument("--texture", help="also write the embedded atlas here as a PNG")
    parser.add_argument("model")
    parser.add_argument("out")
    args = parser.parse_args()

    data = open(args.model, "rb").read()
    magic, version, _ = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67 or version != 2:
        sys.exit("not a glb v2 file")

    json_len, _ = struct.unpack_from("<II", data, 12)
    gltf = json.loads(data[20:20 + json_len])
    bin_off = 20 + json_len
    bin_len, _ = struct.unpack_from("<II", data, bin_off)
    blob = data[bin_off + 8:bin_off + 8 + bin_len]

    mesh = pick_mesh(gltf, args.mesh)
    if len(gltf["meshes"][mesh]["primitives"]) != 1:
        sys.exit("expected one primitive")
    check_nodes(gltf, mesh)

    prim = gltf["meshes"][mesh]["primitives"][0]
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

    with open(args.out, "wb") as out:
        out.write(b"MXR1")
        out.write(struct.pack("<II", len(pos), len(indices)))
        for p, n, t in zip(pos, normal, uv):
            out.write(struct.pack("<8f", *p, *n, *t))
        for i in indices:
            out.write(struct.pack("<H", i))

    tris = len(indices) // 3
    print(f"{args.out}: {len(pos)} vertices, {tris} triangles")

    if args.texture:
        write_texture(gltf, blob, args.texture)


if __name__ == "__main__":
    main()
