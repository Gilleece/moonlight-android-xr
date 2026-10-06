// The .atlas file a baked room's texture ships in: ASTC blocks for every mip
// level, written by tools/atlas_astc.py so the headset neither decodes an
// image nor builds a mip chain. Only the parsing lives here, with no GL in it,
// so it builds and is tested anywhere.
//
// Layout, little endian:
//   char[4]  magic 'MXA1'
//   uint32   width of level 0
//   uint32   height of level 0
//   uint32   block width
//   uint32   block height
//   uint32   level count
//   per level, largest first: uint32 byte length, then that many bytes of
//            ASTC blocks, 16 bytes a block, rows from the top of the picture
//
// Level i is max(1, width >> i) by max(1, height >> i), the size GL expects of
// a mip level, and its length is ceil(w / block width) * ceil(h / block
// height) * 16.

#ifndef XR_ATLAS_H
#define XR_ATLAS_H

#include <stddef.h>
#include <stdint.h>

#define ATLAS_HEADER_BYTES 24
#define ATLAS_BLOCK_BYTES 16
// Past any texture a headset takes, and far enough under what a 32 bit length
// can say that none of the arithmetic below can wrap
#define ATLAS_SIZE_MAX 16384
// A full chain for ATLAS_SIZE_MAX, 16384 down to 1
#define ATLAS_LEVELS_MAX 15

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t blockWidth;
    uint32_t blockHeight;
    int levels;
    // Where each level's blocks start in the file, and how many bytes they are
    size_t offsets[ATLAS_LEVELS_MAX];
    uint32_t lengths[ATLAS_LEVELS_MAX];
} AtlasInfo;

// Reads the header and walks every level against the file's size. The number
// of levels on success, 0 for anything that is not a whole atlas: a bad magic,
// a block size the tool does not write (4x4, 6x6 and 8x8), a size out of
// range, more levels than reach 1x1, or a level whose length is not what its
// size asks for or runs past the end. Bytes past the last level are refused
// too, since they mean the header and the file disagree.
int atlasParse(const unsigned char* data, size_t size, AtlasInfo* out);

// The width and height GL expects of a level
uint32_t atlasLevelSize(uint32_t size, int level);

// What a level of w by h comes to in blocks of bw by bh
uint32_t atlasLevelBytes(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh);

#endif
