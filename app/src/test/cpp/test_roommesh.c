// The .room mesh parser: what a well formed file reads back as, everything it
// has to refuse, and the meshes that ship
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "xr_roommesh.h"

#define ASSETS "../../main/assets/rooms/"

typedef struct {
    uint32_t count;
    int32_t atlas;
} PartSpec;

static void putU32(unsigned char* p, uint32_t v) {
    memcpy(p, &v, sizeof(v));
}

static size_t partAt(uint32_t part) {
    return ROOM_MESH_HEADER_BYTES + (size_t)part * ROOM_MESH_PART_BYTES;
}

// Writes a mesh the way tools/bake_room.py does: the parts one after another
// through the indices, vertex i carrying i in every float so a read can be
// told apart from its neighbours, and indices walking the vertices in turn.
// Returns its size, and the buffer in *out, which the caller frees.
static size_t makeMesh(uint32_t vertices, const PartSpec* parts, uint32_t partCount,
                       uint32_t atlases, unsigned char** out) {
    uint32_t indices = 0;
    for (uint32_t i = 0; i < partCount; i++) {
        indices += parts[i].count;
    }
    size_t vertexAt = partAt(partCount) + ROOM_MESH_CHANNEL_COUNT_BYTES;
    size_t size = vertexAt + (size_t)vertices * ROOM_MESH_VERTEX_FLOATS * 4 + (size_t)indices * 4;
    unsigned char* data = malloc(size);
    memcpy(data, "MXR3", 4);
    putU32(data + 4, vertices);
    putU32(data + 8, indices);
    putU32(data + 12, partCount);
    putU32(data + 16, atlases);
    uint32_t first = 0;
    for (uint32_t i = 0; i < partCount; i++) {
        putU32(data + partAt(i), first);
        putU32(data + partAt(i) + 4, parts[i].count);
        putU32(data + partAt(i) + 8, (uint32_t)parts[i].atlas);
        first += parts[i].count;
    }
    putU32(data + partAt(partCount), 0);
    for (uint32_t v = 0; v < vertices; v++) {
        for (int f = 0; f < ROOM_MESH_VERTEX_FLOATS; f++) {
            float value = (float)v;
            memcpy(data + vertexAt + ((size_t)v * ROOM_MESH_VERTEX_FLOATS + f) * 4, &value, 4);
        }
    }
    size_t indexAt = vertexAt + (size_t)vertices * ROOM_MESH_VERTEX_FLOATS * 4;
    for (uint32_t i = 0; i < indices; i++) {
        putU32(data + indexAt + (size_t)i * 4, i % vertices);
    }
    *out = data;
    return size;
}

static float floatAt(const unsigned char* p) {
    float v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t u32At(const unsigned char* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void testWellFormed(void) {
    unsigned char* data = NULL;
    RoomMeshInfo info;

    // One part painted from one atlas, which is how a baked room comes
    const PartSpec one[] = { { 12, 0 } };
    size_t size = makeMesh(8, one, 1, 1, &data);
    CHECK(roomMeshParse(data, size, &info) == 1);
    CHECK(info.vertexCount == 8 && info.indexCount == 12);
    CHECK(info.partCount == 1 && info.atlasCount == 1);
    CHECK(info.parts[0].firstIndex == 0 && info.parts[0].indexCount == 12);
    CHECK(info.parts[0].atlas == 0);
    CHECK(roomMeshPaintedParts(&info) == 0);
    CHECK(info.vertexOffset == ROOM_MESH_HEADER_BYTES + ROOM_MESH_PART_BYTES + 4);
    CHECK(info.indexOffset == info.vertexOffset + 8 * ROOM_MESH_VERTEX_FLOATS * 4);
    // And the offsets land on the right vertex and the right index
    CHECK(floatAt(data + info.vertexOffset) == 0.0f);
    CHECK(floatAt(data + info.vertexOffset + 3 * ROOM_MESH_VERTEX_FLOATS * 4) == 3.0f);
    CHECK(floatAt(data + info.vertexOffset + (3 * ROOM_MESH_VERTEX_FLOATS + 10) * 4) == 3.0f);
    CHECK(u32At(data + info.indexOffset + 11 * 4) == 3);
    free(data);

    // An atlas part and a vertex colour part, the way the bake splits a
    // primitive that carries both
    const PartSpec two[] = { { 6, 0 }, { 9, -1 } };
    size = makeMesh(10, two, 2, 1, &data);
    CHECK(roomMeshParse(data, size, &info) == 1);
    CHECK(info.partCount == 2);
    CHECK(info.parts[1].firstIndex == 6 && info.parts[1].indexCount == 9);
    CHECK(info.parts[1].atlas == -1);
    CHECK(roomMeshPaintedParts(&info) == 1);
    free(data);

    // Painted throughout, with no atlas at all
    const PartSpec painted[] = { { 3, -1 } };
    size = makeMesh(3, painted, 1, 0, &data);
    CHECK(roomMeshParse(data, size, &info) == 1);
    CHECK(info.atlasCount == 0);
    CHECK(roomMeshPaintedParts(&info) == 1);
    free(data);

    // Every part and every atlas slot the renderer has
    PartSpec many[ROOM_MESH_PARTS_MAX];
    for (int i = 0; i < ROOM_MESH_PARTS_MAX; i++) {
        many[i].count = 3;
        many[i].atlas = i % (ROOM_MESH_ATLASES_MAX + 1) - 1;
    }
    size = makeMesh(4, many, ROOM_MESH_PARTS_MAX, ROOM_MESH_ATLASES_MAX, &data);
    CHECK(roomMeshParse(data, size, &info) == 1);
    CHECK(info.partCount == ROOM_MESH_PARTS_MAX);
    CHECK(info.parts[ROOM_MESH_PARTS_MAX - 1].firstIndex == 3 * (ROOM_MESH_PARTS_MAX - 1));
    free(data);
}

static void testRefused(void) {
    unsigned char* data = NULL;
    RoomMeshInfo info;
    const PartSpec two[] = { { 6, 0 }, { 9, -1 } };
    size_t size = makeMesh(10, two, 2, 1, &data);
    CHECK(roomMeshParse(data, size, &info) == 1);

    // Nothing, and shorter than the header
    CHECK(roomMeshParse(NULL, size, &info) == 0);
    CHECK(roomMeshParse(data, ROOM_MESH_HEADER_BYTES - 1, &info) == 0);

    // The old format, an atlas, and a bad magic
    memcpy(data, "MXR1", 4);
    CHECK(roomMeshParse(data, size, &info) == 0);
    memcpy(data, "MXA1", 4);
    CHECK(roomMeshParse(data, size, &info) == 0);
    CHECK(info.partCount == 0);
    memcpy(data, "MXR3", 4);
    CHECK(roomMeshParse(data, size, &info) == 1);

    // Cut short at the end, inside the part table, and with a byte left over
    CHECK(roomMeshParse(data, size - 1, &info) == 0);
    CHECK(roomMeshParse(data, partAt(1) + 4, &info) == 0);
    CHECK(roomMeshParse(data, partAt(2) + 2, &info) == 0);
    unsigned char* longer = malloc(size + 1);
    memcpy(longer, data, size);
    longer[size] = 0;
    CHECK(roomMeshParse(longer, size + 1, &info) == 0);
    free(longer);

    // Counts of nothing, past the limits, or not whole triangles
    putU32(data + 4, 0);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + 4, ROOM_MESH_VERTS_MAX + 1);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + 4, 0xFFFFFFFFu);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + 4, 10);
    putU32(data + 8, 0);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + 8, 14);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + 8, 0xFFFFFFF0u);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + 8, 15);
    putU32(data + 12, 0);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + 12, ROOM_MESH_PARTS_MAX + 1);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + 12, 2);
    putU32(data + 16, ROOM_MESH_ATLASES_MAX + 1);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + 16, 1);
    CHECK(roomMeshParse(data, size, &info) == 1);

    // A part that is empty, not whole triangles, starts somewhere other than
    // where the one before stopped, or runs past the indices
    putU32(data + partAt(1) + 4, 0);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + partAt(1) + 4, 8);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + partAt(1) + 4, 12);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + partAt(1) + 4, 9);
    putU32(data + partAt(1), 3);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + partAt(1), 9);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + partAt(1), 6);
    CHECK(roomMeshParse(data, size, &info) == 1);

    // Parts that leave indices undrawn
    putU32(data + partAt(1) + 4, 6);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + partAt(1) + 4, 9);

    // An atlas the file does not have, or below the painted marker
    putU32(data + partAt(0) + 8, 1);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + partAt(0) + 8, (uint32_t)-2);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + 16, 0);
    putU32(data + partAt(0) + 8, 0);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + 16, 1);
    CHECK(roomMeshParse(data, size, &info) == 1);

    // Any animation channels
    putU32(data + partAt(2), 1);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + partAt(2), 0);

    // An index past the last vertex, first and last
    size_t indexAt = partAt(2) + 4 + 10 * ROOM_MESH_VERTEX_FLOATS * 4;
    putU32(data + indexAt, 10);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + indexAt, 0);
    putU32(data + indexAt + 14 * 4, 0xFFFFFFFFu);
    CHECK(roomMeshParse(data, size, &info) == 0);
    putU32(data + indexAt + 14 * 4, 9);
    CHECK(roomMeshParse(data, size, &info) == 1);
    free(data);
}

static unsigned char* readFile(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* data = length > 0 ? malloc((size_t)length) : NULL;
    if (data != NULL && fread(data, 1, (size_t)length, f) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (size_t)length;
    return data;
}

// A mesh that ships, as the bake wrote it
static void checkShipped(const char* name, uint32_t vertices, uint32_t triangles,
                         uint32_t parts, int painted, uint32_t atlases) {
    RoomMeshInfo info;
    size_t size = 0;
    char path[256];
    snprintf(path, sizeof(path), ASSETS "%s", name);
    unsigned char* data = readFile(path, &size);
    CHECK(data != NULL);
    if (data == NULL) {
        return;
    }
    CHECK(roomMeshParse(data, size, &info) == 1);
    CHECK(info.vertexCount == vertices);
    CHECK(info.indexCount == triangles * 3);
    CHECK(info.partCount == parts);
    CHECK(roomMeshPaintedParts(&info) == painted);
    CHECK(info.atlasCount == atlases);
    free(data);
}

static void testShipped(void) {
    checkShipped("home_theater.room", 16821, 5876, 1, 0, 1);
    checkShipped("grand_cinema.room", 43670, 17790, 1, 0, 1);
    // The sofa off the atlas, and the rest painted from its colours
    checkShipped("synthwave.room", 26169, 12958, 2, 1, 1);
}

int main(void) {
    testWellFormed();
    testRefused();
    testShipped();
    return checksDone("xr_roommesh");
}
