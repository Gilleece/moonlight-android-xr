// The .room mesh parser. See xr_roommesh.h for the layout.
#include <string.h>

#include "xr_roommesh.h"

static uint32_t readU32(const unsigned char* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

int roomMeshParse(const unsigned char* data, size_t size, RoomMeshInfo* out) {
    memset(out, 0, sizeof(*out));
    if (data == NULL || size < ROOM_MESH_HEADER_BYTES || memcmp(data, "MXR3", 4) != 0) {
        return 0;
    }
    uint32_t vertexCount = readU32(data + 4);
    uint32_t indexCount = readU32(data + 8);
    uint32_t partCount = readU32(data + 12);
    uint32_t atlasCount = readU32(data + 16);
    // Everything is held to what the file could possibly hold before any of
    // the byte counts are worked out, so none of the arithmetic below can wrap
    size_t payload = size - ROOM_MESH_HEADER_BYTES;
    if (vertexCount == 0 || vertexCount > ROOM_MESH_VERTS_MAX
            || indexCount == 0 || indexCount % 3 != 0 || indexCount > payload / 4
            || partCount == 0 || partCount > ROOM_MESH_PARTS_MAX
            || atlasCount > ROOM_MESH_ATLASES_MAX) {
        return 0;
    }

    size_t channelAt = ROOM_MESH_HEADER_BYTES + (size_t)partCount * ROOM_MESH_PART_BYTES;
    if (size < channelAt + ROOM_MESH_CHANNEL_COUNT_BYTES) {
        return 0;
    }
    uint32_t drawn = 0;
    for (uint32_t i = 0; i < partCount; i++) {
        const unsigned char* record = data + ROOM_MESH_HEADER_BYTES + i * ROOM_MESH_PART_BYTES;
        uint32_t first = readU32(record);
        uint32_t count = readU32(record + 4);
        int32_t atlas = (int32_t)readU32(record + 8);
        if (count == 0 || count % 3 != 0 || first != drawn || count > indexCount - drawn
                || atlas < -1 || atlas >= (int32_t)atlasCount) {
            return 0;
        }
        out->parts[i].firstIndex = first;
        out->parts[i].indexCount = count;
        out->parts[i].atlas = atlas;
        drawn += count;
    }
    if (drawn != indexCount) {
        return 0;
    }
    if (readU32(data + channelAt) != 0) {
        return 0;
    }

    size_t vertexAt = channelAt + ROOM_MESH_CHANNEL_COUNT_BYTES;
    size_t vertexBytes = (size_t)vertexCount * ROOM_MESH_VERTEX_FLOATS * sizeof(float);
    size_t indexBytes = (size_t)indexCount * sizeof(uint32_t);
    if (size - vertexAt != vertexBytes + indexBytes) {
        return 0;
    }
    size_t indexAt = vertexAt + vertexBytes;
    for (uint32_t i = 0; i < indexCount; i++) {
        if (readU32(data + indexAt + (size_t)i * 4) >= vertexCount) {
            return 0;
        }
    }

    out->vertexCount = vertexCount;
    out->indexCount = indexCount;
    out->partCount = partCount;
    out->atlasCount = atlasCount;
    out->vertexOffset = vertexAt;
    out->indexOffset = indexAt;
    return 1;
}

int roomMeshPaintedParts(const RoomMeshInfo* info) {
    int painted = 0;
    for (uint32_t i = 0; i < info->partCount; i++) {
        if (info->parts[i].atlas < 0) {
            painted++;
        }
    }
    return painted;
}
