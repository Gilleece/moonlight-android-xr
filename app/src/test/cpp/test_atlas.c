// The .atlas parser: what a well formed file reads back as, everything it
// has to refuse, and the two atlases that ship
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "xr_atlas.h"

#define ASSETS "../../main/assets/rooms/"

static void putU32(unsigned char* p, uint32_t v) {
    memcpy(p, &v, sizeof(v));
}

// Writes an atlas the way tools/atlas_astc.py does, with levels of garbage
// blocks the parser never looks inside. Returns its size, and the buffer in
// *out, which the caller frees.
static size_t makeAtlas(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint32_t levels,
                        unsigned char** out) {
    size_t size = ATLAS_HEADER_BYTES;
    for (uint32_t i = 0; i < levels; i++) {
        size += 4 + atlasLevelBytes(atlasLevelSize(w, (int)i), atlasLevelSize(h, (int)i),
                                    bw, bh);
    }
    unsigned char* data = malloc(size);
    memcpy(data, "MXA1", 4);
    putU32(data + 4, w);
    putU32(data + 8, h);
    putU32(data + 12, bw);
    putU32(data + 16, bh);
    putU32(data + 20, levels);
    size_t at = ATLAS_HEADER_BYTES;
    for (uint32_t i = 0; i < levels; i++) {
        uint32_t length = atlasLevelBytes(atlasLevelSize(w, (int)i),
                                          atlasLevelSize(h, (int)i), bw, bh);
        putU32(data + at, length);
        memset(data + at + 4, (int)(0x40 + i), length);
        at += 4 + length;
    }
    *out = data;
    return size;
}

static void testLevelMaths(void) {
    CHECK(atlasLevelSize(4096, 0) == 4096);
    CHECK(atlasLevelSize(4096, 12) == 1);
    CHECK(atlasLevelSize(64, 3) == 8);
    // Past the end of the chain a side stays at 1 rather than reaching 0
    CHECK(atlasLevelSize(32, 9) == 1);
    CHECK(atlasLevelSize(37, 1) == 18);
    // Partial blocks round up
    CHECK(atlasLevelBytes(4096, 4096, 6, 6) == 683u * 683u * 16u);
    CHECK(atlasLevelBytes(1, 1, 6, 6) == 16);
    CHECK(atlasLevelBytes(37, 19, 4, 4) == 10u * 5u * 16u);
}

static void testWellFormed(void) {
    unsigned char* data = NULL;
    AtlasInfo info;

    // A full 6x6 chain down to 1x1 on a square
    size_t size = makeAtlas(64, 64, 6, 6, 7, &data);
    CHECK(atlasParse(data, size, &info) == 7);
    CHECK(info.width == 64 && info.height == 64);
    CHECK(info.blockWidth == 6 && info.blockHeight == 6);
    CHECK(info.levels == 7);
    CHECK(info.offsets[0] == ATLAS_HEADER_BYTES + 4);
    CHECK(info.lengths[0] == 11u * 11u * 16u);
    CHECK(info.lengths[6] == 16);
    // Each level starts right after the one before it and its own length
    int contiguous = 1;
    for (int i = 1; i < info.levels; i++) {
        contiguous &= info.offsets[i] == info.offsets[i - 1] + info.lengths[i - 1] + 4;
    }
    CHECK(contiguous);
    // And the offsets really land on that level's blocks
    CHECK(data[info.offsets[0]] == 0x40);
    CHECK(data[info.offsets[3]] == 0x43);
    CHECK(data[info.offsets[6] + 15] == 0x46);
    free(data);

    // A wide one runs as many levels as its longer side takes
    size = makeAtlas(128, 32, 8, 8, 8, &data);
    CHECK(atlasParse(data, size, &info) == 8);
    CHECK(info.lengths[7] == 16);
    free(data);

    // A chain that stops short of 1x1 is still a whole texture to GL
    size = makeAtlas(256, 256, 4, 4, 3, &data);
    CHECK(atlasParse(data, size, &info) == 3);
    free(data);
}

static void testRefused(void) {
    unsigned char* data = NULL;
    AtlasInfo info;
    size_t size = makeAtlas(64, 64, 6, 6, 7, &data);

    // Nothing, and shorter than the header
    CHECK(atlasParse(NULL, size, &info) == 0);
    CHECK(atlasParse(data, ATLAS_HEADER_BYTES - 1, &info) == 0);

    // A bad magic, and the room model's magic in particular
    memcpy(data, "MXA2", 4);
    CHECK(atlasParse(data, size, &info) == 0);
    memcpy(data, "MXR1", 4);
    CHECK(atlasParse(data, size, &info) == 0);
    CHECK(info.levels == 0);
    memcpy(data, "MXA1", 4);
    CHECK(atlasParse(data, size, &info) == 7);

    // Cut short inside the last level, inside a length, and with bytes left over
    CHECK(atlasParse(data, size - 1, &info) == 0);
    CHECK(atlasParse(data, ATLAS_HEADER_BYTES + 2, &info) == 0);
    unsigned char* longer = malloc(size + 1);
    memcpy(longer, data, size);
    longer[size] = 0;
    CHECK(atlasParse(longer, size + 1, &info) == 0);
    free(longer);

    // A level whose length is not what its size asks for
    putU32(data + ATLAS_HEADER_BYTES, 11u * 11u * 16u + 16u);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + ATLAS_HEADER_BYTES, 11u * 11u * 16u);
    CHECK(atlasParse(data, size, &info) == 7);

    // More levels than the chain has, none at all, or past the limit
    putU32(data + 20, 8);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 20, 0);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 20, ATLAS_LEVELS_MAX + 1);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 20, 7);

    // Block sizes the tool never writes, square or not
    putU32(data + 12, 5);
    putU32(data + 16, 5);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 12, 6);
    putU32(data + 16, 8);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 12, 0);
    putU32(data + 16, 0);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 12, 6);
    putU32(data + 16, 6);

    // A side of nothing, or past the largest texture
    putU32(data + 4, 0);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 4, ATLAS_SIZE_MAX + 1);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 4, 0xFFFFFFFFu);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 4, 64);
    CHECK(atlasParse(data, size, &info) == 7);
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

// One atlas that ships, as the tool wrote it: the 4096 set every headset but
// the XR2 Gen 1 reads, or the 2048 set those read
static void checkShipped(const char* name, uint32_t side, int levels) {
    AtlasInfo info;
    size_t size = 0;
    char path[256];
    snprintf(path, sizeof(path), ASSETS "%s", name);
    unsigned char* data = readFile(path, &size);
    CHECK(data != NULL);
    if (data == NULL) {
        return;
    }
    CHECK(atlasParse(data, size, &info) == levels);
    CHECK(info.width == side && info.height == side);
    CHECK(info.blockWidth == 6 && info.blockHeight == 6);
    CHECK(info.lengths[0] == atlasLevelBytes(side, side, 6, 6));
    CHECK(info.lengths[levels - 1] == 16);
    free(data);
}

static void testShipped(void) {
    const char* rooms[] = { "home_theater", "grand_cinema", "synthwave" };
    for (int i = 0; i < 3; i++) {
        char name[64];
        snprintf(name, sizeof(name), "%s_0.atlas", rooms[i]);
        checkShipped(name, 4096, 13);
        snprintf(name, sizeof(name), "%s_lo_0.atlas", rooms[i]);
        checkShipped(name, 2048, 12);
    }
}

int main(void) {
    testLevelMaths();
    testWellFormed();
    testRefused();
    testShipped();
    return checksDone("xr_atlas");
}
