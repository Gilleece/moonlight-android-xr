// The .atlas parser. See xr_atlas.h for the layout.
#include <string.h>

#include "xr_atlas.h"

static uint32_t readU32(const unsigned char* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static int blockSupported(uint32_t bw, uint32_t bh) {
    return bw == bh && (bw == 4 || bw == 6 || bw == 8);
}

uint32_t atlasLevelSize(uint32_t size, int level) {
    uint32_t s = size >> level;
    return s > 0 ? s : 1;
}

uint32_t atlasLevelBytes(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh) {
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * ATLAS_BLOCK_BYTES;
}

int atlasParse(const unsigned char* data, size_t size, AtlasInfo* out) {
    memset(out, 0, sizeof(*out));
    if (data == NULL || size < ATLAS_HEADER_BYTES || memcmp(data, "MXA1", 4) != 0) {
        return 0;
    }
    uint32_t width = readU32(data + 4);
    uint32_t height = readU32(data + 8);
    uint32_t bw = readU32(data + 12);
    uint32_t bh = readU32(data + 16);
    uint32_t count = readU32(data + 20);
    if (!blockSupported(bw, bh) || width == 0 || height == 0
            || width > ATLAS_SIZE_MAX || height > ATLAS_SIZE_MAX
            || count == 0 || count > ATLAS_LEVELS_MAX) {
        return 0;
    }
    // No more levels than it takes the longer side to reach 1
    uint32_t longer = width > height ? width : height;
    uint32_t chain = 1;
    while ((longer >> chain) > 0) {
        chain++;
    }
    if (count > chain) {
        return 0;
    }

    size_t at = ATLAS_HEADER_BYTES;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t expect = atlasLevelBytes(atlasLevelSize(width, (int)i),
                                          atlasLevelSize(height, (int)i), bw, bh);
        if (size - at < 4) {
            return 0;
        }
        uint32_t length = readU32(data + at);
        at += 4;
        if (length != expect || size - at < length) {
            return 0;
        }
        out->offsets[i] = at;
        out->lengths[i] = length;
        at += length;
    }
    if (at != size) {
        return 0;
    }

    out->width = width;
    out->height = height;
    out->blockWidth = bw;
    out->blockHeight = bh;
    out->levels = (int)count;
    return out->levels;
}
