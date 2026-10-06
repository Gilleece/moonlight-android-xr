// The .room file a baked room's mesh ships in, written by tools/bake_room.py.
// Only the parsing lives here, with no GL in it, so it builds and is tested
// anywhere.
//
// Layout, little endian:
//   char[4]  magic 'MXR3'
//   uint32   vertex count
//   uint32   index count
//   uint32   part count
//   uint32   atlas count
//   per part: uint32 first index, uint32 index count, int32 atlas, -1 for a
//            part painted from its vertex colours
//   uint32   channel count
//   float[11] per vertex: position xyz, normal xyz, uv, rgb
//   uint32   indices
//
// The parts run through the indices in order, each starting where the one
// before it stopped, and between them they draw every triangle once. A part
// is painted from its atlas or from its vertex colours and never both.
//
// The format can carry node animation channels between the part table and the
// vertices. None are played here and a file with any is refused: a part one
// drives is baked in its node's own space, so drawn without it the part would
// sit somewhere else entirely.

#ifndef XR_ROOMMESH_H
#define XR_ROOMMESH_H

#include <stddef.h>
#include <stdint.h>

// The header, then one part record, then the channel count after the parts
#define ROOM_MESH_HEADER_BYTES 20
#define ROOM_MESH_PART_BYTES 12
#define ROOM_MESH_CHANNEL_COUNT_BYTES 4
// Position, normal, texture coordinate and colour
#define ROOM_MESH_VERTEX_FLOATS 11
// Far past any room that ships, and far enough under what a 32 bit count can
// say that none of the arithmetic below can wrap
#define ROOM_MESH_VERTS_MAX (1 << 20)
#define ROOM_MESH_PARTS_MAX 16
#define ROOM_MESH_ATLASES_MAX 4

typedef struct {
    uint32_t firstIndex;
    uint32_t indexCount;
    // The atlas slot the part is painted from, or -1 for its vertex colours
    int32_t atlas;
} RoomMeshPart;

typedef struct {
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t partCount;
    uint32_t atlasCount;
    RoomMeshPart parts[ROOM_MESH_PARTS_MAX];
    // Where the vertices and the indices start in the file
    size_t vertexOffset;
    size_t indexOffset;
} RoomMeshInfo;

// Reads the header and the part table and checks the whole file against them.
// 1 for a mesh the renderer can draw, 0 for anything else: a bad magic, no
// vertices or past the limit, no indices or not whole triangles, no parts or
// more than the limit, more atlases than the limit, a part that is empty, not
// whole triangles or not where the one before it stopped, parts that leave
// indices undrawn, a part naming an atlas the file does not have, any
// channels, a size that is not exactly what the counts ask for, or an index
// past the last vertex.
int roomMeshParse(const unsigned char* data, size_t size, RoomMeshInfo* out);

// How many of a parsed mesh's parts are painted from their vertex colours
int roomMeshPaintedParts(const RoomMeshInfo* info);

#endif
