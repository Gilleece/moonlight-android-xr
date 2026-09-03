// The 3d rooms: a generated shell and a baked model, both drawn per eye
// into the one projection layer this renderer has, with the picture hung
// on the far wall.
#include "xr_renderer.h"
#include "xr_shaders.h"

// Everything the room's shape and colouring is made of, gathered in one place
// so the look can be changed without reading the generator
typedef struct {
    float halfWidth;
    float floorY;
    // The floor under the picture, which is the one it must not hang through.
    // The same as floorY in a room with one level, lower in a raked one, where
    // floorY is the tier the viewer stands on.
    float screenFloorY;
    float ceilingY;
    // The wall the picture hangs on, and the one behind the viewer
    float screenZ;
    float backZ;
    // Quads per side on each face, so a face carries this squared of them
    int subdiv;
    // What each kind of surface is painted before the gradients go on
    float wallLevel;
    float floorLevel;
    float ceilingLevel;
    // Where the picture hangs, which is what the light is baked from
    Vec3 screenAt;
    // How high on the wall the picture is mounted, and how far off the wall it
    // stands so the two never fight for the same pixels
    float screenMountY;
    float screenProud;
    // How wide it is hung. The room sizes its own picture rather than taking
    // the size slider's, since the wall it goes on is a known size.
    float screenWidth;
    // Distance at which the screen's light is down to half
    float spillRadius;
    // How much of that light a fully lit vertex takes
    float spillGain;
    // 0 for a room painted by the generator, 1 for one taking its colour off a
    // texture atlas, and how far down that atlas is turned on the way in
    float texMix;
    float dim;
    unsigned seed;
} RoomParams;

// A dither of about one 255th, from the seed and the vertex number. Without it
// the wall gradients are shallow enough over enough pixels to band.
static float roomDither(unsigned seed, unsigned index) {
    unsigned h = seed + index * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return ((float)(h & 0xffffu) / 65535.0f - 0.5f) * (2.0f / 255.0f);
}

// What a point on the room is painted, before any of the screen's light lands
// on it. Near black throughout: everything here is a gradient between shades
// of almost nothing, and the picture is what the eye should be adapting to.
static void roomVertexColor(const RoomParams* p, int surface, Vec3 pos, float* rgb) {
    // 0 at the screen wall, 1 at the back of the room
    float back = (pos.z - p->screenZ) / (p->backZ - p->screenZ);
    // 0 on the floor, 1 at the ceiling
    float up = (pos.y - p->floorY) / (p->ceilingY - p->floorY);
    // 0 down the middle, 1 at either side wall
    float side = fabsf(pos.x) / p->halfWidth;

    if (surface == ROOM_SURF_FLOOR) {
        float level = p->floorLevel * (1.0f - 0.35f * back * back);
        // A shade warmer and a shade lighter than the walls
        rgb[0] = level * 1.00f;
        rgb[1] = level * 0.93f;
        rgb[2] = level * 0.84f;
        return;
    }

    float level;
    if (surface == ROOM_SURF_CEILING) {
        level = p->ceilingLevel * (1.0f - 0.30f * back);
    }
    else {
        // Darker toward the ceiling and darker again into the rear corners,
        // where a real room has nothing lighting it at all
        level = p->wallLevel * (1.0f - 0.45f * up)
                * (1.0f - 0.30f * back * back * (0.4f + 0.6f * side));
    }
    rgb[0] = level;
    rgb[1] = level;
    rgb[2] = level;
}

// How much of the picture's light reaches a point on the room. Baked from a
// single point where the screen sits by default: a distance and a facing term
// is as much of a light that size as a dark wall ever shows.
static float roomSpillWeight(const RoomParams* p, Vec3 pos, Vec3 normal) {
    Vec3 toScreen = vecSub(p->screenAt, pos);
    float dist = sqrtf(vecDot(toScreen, toScreen));
    if (dist < 1e-4f) {
        return 1.0f;
    }
    Vec3 dir = { toScreen.x / dist, toScreen.y / dist, toScreen.z / dist };
    float facing = vecDot(normal, dir);
    if (facing <= 0.0f) {
        return 0.0f;
    }

    float ratio = dist / p->spillRadius;
    float weight = facing / (1.0f + ratio * ratio);

    // Nothing behind the viewer catches any of it, faded in over the back half
    // of the room so the falloff has no edge in it
    if (pos.z > 0.0f && p->backZ > 0.0f) {
        float behind = 1.0f - pos.z / p->backZ;
        weight *= behind > 0.0f ? behind : 0.0f;
    }
    return weight;
}

// Writes one vertex in the layout the room's buffer is in. A generated room has
// no atlas behind it, so it passes 0,0 for the texture coordinate and the
// shader mixes it out.
static void roomWriteVertex(const RoomParams* p, float* verts, int index, Vec3 pos,
                            const float* rgb, float spill, float u, float v) {
    float dither = roomDither(p->seed, (unsigned)index);
    float* out = verts + (size_t)index * ROOM_VERTEX_FLOATS;
    out[0] = pos.x;
    out[1] = pos.y;
    out[2] = pos.z;
    out[3] = rgb[0] + dither;
    out[4] = rgb[1] + dither;
    out[5] = rgb[2] + dither;
    out[6] = spill;
    out[7] = u;
    out[8] = v;
    out[9] = 0.0f;
}

// The most a room can ask for, so a caller can size its buffers without
// knowing how the room is put together
static void roomMaxCounts(const RoomParams* p, int* maxVerts, int* maxIndices) {
    *maxVerts = ROOM_FACES * (p->subdiv + 1) * (p->subdiv + 1);
    *maxIndices = ROOM_FACES * p->subdiv * p->subdiv * 6;
}

// Builds the whole room, vertices and indices, into buffers the caller owns.
// The generated styles only, which is the bare shell: a baked room comes out of
// its own model file instead.
//
// Triangles are wound counter clockwise seen from inside the box, so the side
// the viewer is on faces front. Culling is left off all the same, since nothing
// in here is ever seen from behind and there is nothing for a cull to save.
static int buildRoomGeometry(const RoomParams* p, int style, float* verts, int maxVerts,
                             unsigned short* indices, int maxIndices,
                             int* vertexCount, int* indexCount) {
    int n = p->subdiv;
    if (n < 1 || style < ROOM_STYLE_MINIMAL) {
        return 0;
    }
    int needVerts = 0;
    int needIndices = 0;
    roomMaxCounts(p, &needVerts, &needIndices);
    // Past the index type is a badly chosen parameter rather than a room worth
    // drawing, so it fails here the same way a short buffer does
    if (needVerts > ROOM_MAX_VERTS || needVerts > maxVerts || needIndices > maxIndices) {
        return 0;
    }

    float width = p->halfWidth * 2.0f;
    float height = p->ceilingY - p->floorY;
    float depth = p->backZ - p->screenZ;

    // Origin and the two edges each face is swept along, ordered so that the
    // cross product of the two points into the room
    struct {
        Vec3 origin;
        Vec3 edgeU;
        Vec3 edgeV;
        int surface;
    } faces[ROOM_FACES] = {
        // The wall the picture hangs on
        { { -p->halfWidth, p->floorY, p->screenZ },
          { width, 0.0f, 0.0f }, { 0.0f, height, 0.0f }, ROOM_SURF_WALL },
        // Behind the viewer
        { { p->halfWidth, p->floorY, p->backZ },
          { -width, 0.0f, 0.0f }, { 0.0f, height, 0.0f }, ROOM_SURF_WALL },
        { { -p->halfWidth, p->floorY, p->screenZ },
          { 0.0f, height, 0.0f }, { 0.0f, 0.0f, depth }, ROOM_SURF_WALL },
        { { p->halfWidth, p->floorY, p->screenZ },
          { 0.0f, 0.0f, depth }, { 0.0f, height, 0.0f }, ROOM_SURF_WALL },
        { { -p->halfWidth, p->floorY, p->screenZ },
          { 0.0f, 0.0f, depth }, { width, 0.0f, 0.0f }, ROOM_SURF_FLOOR },
        { { -p->halfWidth, p->ceilingY, p->screenZ },
          { width, 0.0f, 0.0f }, { 0.0f, 0.0f, depth }, ROOM_SURF_CEILING },
    };

    int written = 0;
    int used = 0;
    for (int f = 0; f < ROOM_FACES; f++) {
        Vec3 normal = vecNorm(vecCross(faces[f].edgeU, faces[f].edgeV));
        int base = written;

        for (int j = 0; j <= n; j++) {
            for (int i = 0; i <= n; i++) {
                float u = (float)i / (float)n;
                float v = (float)j / (float)n;
                Vec3 pos = {
                    faces[f].origin.x + faces[f].edgeU.x * u + faces[f].edgeV.x * v,
                    faces[f].origin.y + faces[f].edgeU.y * u + faces[f].edgeV.y * v,
                    faces[f].origin.z + faces[f].edgeU.z * u + faces[f].edgeV.z * v,
                };

                float rgb[3];
                roomVertexColor(p, faces[f].surface, pos, rgb);
                roomWriteVertex(p, verts, written, pos, rgb,
                                roomSpillWeight(p, pos, normal), 0.0f, 0.0f);
                written++;
            }
        }

        for (int j = 0; j < n; j++) {
            for (int i = 0; i < n; i++) {
                unsigned short a = (unsigned short)(base + j * (n + 1) + i);
                unsigned short b = (unsigned short)(a + 1);
                unsigned short c = (unsigned short)(a + n + 1);
                unsigned short d = (unsigned short)(c + 1);
                indices[used++] = a;
                indices[used++] = b;
                indices[used++] = d;
                indices[used++] = a;
                indices[used++] = d;
                indices[used++] = c;
            }
        }
    }

    *vertexCount = written;
    *indexCount = used;
    return 1;
}

// Which room is in force, the picker's unless the debug property has taken it
// over. 0 is no room at all, which is every other environment.
int roomEffective(XrCtx* ctx) {
    return ctx->roomOverride >= 0 ? ctx->roomOverride : ctx->roomStyle;
}

// The room as it stands, which the generator and the shipped look both come
// out of. Metres, and the origin is where the viewer starts. Bare walls with
// nothing in them: the picture is the only thing here worth looking at.
static RoomParams minimalRoomParams(void) {
    RoomParams p;
    memset(&p, 0, sizeof(p));
    p.halfWidth = 4.5f;
    p.floorY = -1.4f;
    // One level throughout, so the picture stands on the same floor as the
    // viewer
    p.screenFloorY = p.floorY;
    p.ceilingY = 3.0f;
    p.screenZ = -5.5f;
    p.backZ = 4.0f;
    // Eight quads a side keeps the gradients smooth across a nine metre wall
    // for a few hundred vertices in total
    p.subdiv = 8;
    p.wallLevel = 0.055f;
    p.floorLevel = 0.065f;
    p.ceilingLevel = 0.038f;
    // Where the picture is hung, and the point the spill is baked from. The
    // bake point sits proud of the wall the way the screen does, so the wall
    // behind the picture catches some of its light too.
    p.screenMountY = 0.6f;
    p.screenProud = 0.10f;
    // Half again the 3 m the sliders start on. A wall nine metres across can
    // carry it, and at this distance it is what the room is for.
    p.screenWidth = 4.5f;
    Vec3 screenAt = { 0.0f, p.screenMountY, p.screenZ + p.screenProud };
    p.screenAt = screenAt;
    p.spillRadius = 2.2f;
    p.spillGain = 0.30f;
    p.seed = 0x9e3779b9u;
    return p;
}

// How high the viewer anchor sits in the model's own space at a given scale.
// The tier under them lands at eye height whatever the room is scaled to, so
// the floor stays where it is while the walls come in and out around it.
static float roomModelAnchorY(float scale) {
    return ROOM_MODEL_TIER_Y + ROOM_EYE_HEIGHT_M / scale;
}

// The baked cinema, measured off the model and put through the same
// (model - anchor) * scale the geometry is, so the screen hangs in the
// proscenium at every scale. The screen sits in the recess behind the curtains,
// so it needs nothing standing it off the wall, and the whole of it is
// textured, so the surface levels and the subdiv the generator works from are
// unused here.
static RoomParams psxCinemaParams(float scale) {
    float anchorY = roomModelAnchorY(scale);
    RoomParams p;
    memset(&p, 0, sizeof(p));
    p.halfWidth = 15.47f * scale;
    // The seating tier the viewer stands on, which the anchor holds at eye
    // height, and the ceiling over the stalls
    p.floorY = -ROOM_EYE_HEIGHT_M;
    // The stage floor at the far wall, model y -4.25, which is a good way below
    // the tier and is what the picture has to clear
    p.screenFloorY = (-4.25f - anchorY) * scale;
    p.ceilingY = (8.57f - anchorY) * scale;
    // The screen wall is at model z -27.53, and the picture hangs 0.18 proud of
    // it, so model -27.35 through the anchor at -12
    p.screenZ = -15.35f * scale;
    p.backZ = 14.33f * scale;
    // The centre of the proscenium opening is model y 2.85, and the picture
    // hangs 15 percent of its own height under that: the opening is 18 model
    // units across, so 10.125 high at 16:9, and 2.85 - 0.15 * 10.125 is 1.33
    p.screenMountY = (1.33f - anchorY) * scale;
    p.screenProud = 0.0f;
    // The opening is 20 m across at full size, so this fills it with a margin
    // either side
    p.screenWidth = 18.0f * scale;
    Vec3 screenAt = { 0.0f, p.screenMountY, p.screenZ };
    p.screenAt = screenAt;
    // A room this size takes the light much further than the small one. It
    // takes less of it per surface than a painted wall would, since the atlas
    // is already carrying the colour, but not as little as it first shipped
    // with: over a textured surface a fifth of the frame's colour never read
    // as light at all.
    p.spillRadius = 7.0f * scale;
    p.spillGain = 0.55f;
    p.texMix = 1.0f;
    // The atlas is already painted as an interior with the lights down, so it
    // goes on as it was baked
    p.dim = 1.0f;
    p.seed = 0x85ebca6bu;
    return p;
}

// Which room a style asks for, at the scale that style is drawn. Anything
// unknown falls back to the generated one rather than leaving the buffers empty.
static RoomParams roomParams(int style, float scale) {
    if (style == ROOM_STYLE_PSX) {
        return psxCinemaParams(scale);
    }
    return minimalRoomParams();
}

// How large a style is drawn. Only the baked room is scaled: the generated one
// is built at the size its own params give. A property set inside the range
// wins over the shipped default.
static float roomScale(XrCtx* ctx, int style) {
    if (style != ROOM_STYLE_PSX) {
        return 1.0f;
    }
    float scale = ctx->roomScaleOverride > 0.0f ? ctx->roomScaleOverride : ROOM_PSX_SCALE;
    if (scale < ROOM_SCALE_MIN) {
        scale = ROOM_SCALE_MIN;
    }
    if (scale > ROOM_SCALE_MAX) {
        scale = ROOM_SCALE_MAX;
    }
    return scale;
}

// How far down the atlas is turned as the room draws. Nothing is baked into the
// geometry from this, so the property moves it frame to frame with no rebuild
// behind it, and it wins over whatever the built style left in place.
static float roomDim(XrCtx* ctx) {
    if (ctx->roomDimOverride <= 0.0f) {
        return ctx->roomDim;
    }
    float dim = ctx->roomDimOverride;
    if (dim < ROOM_DIM_MIN) {
        dim = ROOM_DIM_MIN;
    }
    if (dim > ROOM_DIM_MAX) {
        dim = ROOM_DIM_MAX;
    }
    return dim;
}

// In a 3d room the picture hangs on the far wall, so the placement the sliders
// and the grab produce is put aside on the way in and handed back on the way
// out. Nothing is written to preferences either way: what the user set up in a
// normal environment is still there when they come back to one.
void applyRoomPlacement(XrCtx* ctx, int style, float aspect, int reseeded) {
    int roomOn = style > 0;
    if (roomOn && !ctx->roomHoldingScreen) {
        ctx->savedScreenPose = ctx->screenPose;
        ctx->savedScreenWidth = ctx->screenWidth;
        ctx->savedScreenRadius = ctx->screenRadius;
        ctx->roomHoldingScreen = 1;
        // Anything held would spend the rest of the drag fighting the wall
        ctx->grabMode = GRAB_NONE;
    }
    else if (roomOn && reseeded) {
        // The panel's reset landed while the room had the screen. What it
        // seeded is the placement that should be waiting when the room ends.
        ctx->savedScreenPose = ctx->screenPose;
        ctx->savedScreenWidth = ctx->screenWidth;
        ctx->savedScreenRadius = ctx->screenRadius;
    }
    else if (!roomOn && ctx->roomHoldingScreen) {
        ctx->screenPose = ctx->savedScreenPose;
        ctx->screenWidth = ctx->savedScreenWidth;
        ctx->screenRadius = ctx->savedScreenRadius;
        ctx->roomHoldingScreen = 0;
    }
    if (!roomOn) {
        return;
    }

    // The same scale the geometry was built at, so the picture and the walls
    // around it never disagree
    RoomParams p = roomParams(style, roomScale(ctx, style));
    // The room says how big its picture is, not the size slider: the wall is a
    // known size and the picture is hung to suit it. The clamps below only
    // catch a room whose width does not fit its own wall.
    float width = p.screenWidth;
    float maxWidth = 2.0f * p.halfWidth - 0.4f;
    float maxHeight = (p.ceilingY - p.floorY) - 0.3f;
    if (width > maxWidth) {
        width = maxWidth;
    }
    if (width * aspect > maxHeight) {
        width = maxHeight / aspect;
    }
    float height = width * aspect;
    // And hung where the whole of it is on the wall rather than through the
    // floor or the ceiling. The floor here is the one under the picture, not
    // the tier the viewer is on, which in a raked room is metres higher and
    // would push the picture back up the wall.
    float mount = p.screenMountY;
    float lowest = p.screenFloorY + height * 0.5f + 0.1f;
    float highest = p.ceilingY - height * 0.5f - 0.1f;
    if (mount < lowest) {
        mount = lowest;
    }
    if (mount > highest) {
        mount = highest;
    }

    // Square to the wall and facing the viewer, the same identity orientation
    // the placement starts out with
    memset(&ctx->screenPose, 0, sizeof(ctx->screenPose));
    ctx->screenPose.orientation.w = 1.0f;
    ctx->screenPose.position.y = mount;
    ctx->screenPose.position.z = p.screenZ + p.screenProud;
    ctx->screenWidth = width;
}

// Whether the assets a baked room is made of have both arrived
static int roomAssetsReady(XrCtx* ctx) {
    return ctx->roomModelReady && ctx->roomTextureReady;
}

// Turns the loaded model into the layout the room's buffer is in. Nothing is
// generated here beyond the light: the shape and the texture coordinates come
// off the model, and the colour is mixed out by the atlas. The model arrives in
// its own space, so this is where the anchor and the scale go on. The normals
// are left alone, since a uniform scale does not turn them.
static int buildModelRoomGeometry(XrCtx* ctx, const RoomParams* p, float scale, float* verts,
                                  unsigned short* indices, int* vertexCount, int* indexCount) {
    if (!ctx->roomModelReady) {
        return 0;
    }
    float anchorY = roomModelAnchorY(scale);
    static const float white[3] = { 1.0f, 1.0f, 1.0f };
    for (int i = 0; i < ctx->roomModelVertexCount; i++) {
        const float* src = ctx->roomModelVerts + (size_t)i * ROOM_MODEL_FLOATS;
        Vec3 pos = { (src[0] - ROOM_MODEL_ANCHOR_X) * scale,
                     (src[1] - anchorY) * scale,
                     (src[2] - ROOM_MODEL_ANCHOR_Z) * scale };
        Vec3 normal = { src[3], src[4], src[5] };
        roomWriteVertex(p, verts, i, pos, white, roomSpillWeight(p, pos, normal),
                        src[6], src[7]);
    }
    memcpy(indices, ctx->roomModelIndices,
           (size_t)ctx->roomModelIndexCount * sizeof(unsigned short));
    *vertexCount = ctx->roomModelVertexCount;
    *indexCount = ctx->roomModelIndexCount;
    return 1;
}

// Builds a style's room and hands it to the buffers. Called once for the first
// room and again whenever the picker moves to another: a one off pass over a
// few thousand vertices, which is cheaper than keeping every room resident for
// a switch that may never come.
static int uploadRoomGeometry(XrCtx* ctx, int style) {
    float scale = roomScale(ctx, style);
    RoomParams params = roomParams(style, scale);
    int baked = style == ROOM_STYLE_PSX;
    if (baked && !roomAssetsReady(ctx)) {
        return 0;
    }
    int maxVerts = 0;
    int maxIndices = 0;
    if (baked) {
        maxVerts = ctx->roomModelVertexCount;
        maxIndices = ctx->roomModelIndexCount;
    }
    else {
        roomMaxCounts(&params, &maxVerts, &maxIndices);
    }
    float* verts = malloc((size_t)maxVerts * ROOM_VERTEX_FLOATS * sizeof(float));
    unsigned short* indices = malloc((size_t)maxIndices * sizeof(unsigned short));
    if (verts == NULL || indices == NULL) {
        free(verts);
        free(indices);
        LOGE("room geometry allocation failed");
        return 0;
    }

    int vertexCount = 0;
    int indexCount = 0;
    int ok = baked
            ? buildModelRoomGeometry(ctx, &params, scale, verts, indices, &vertexCount, &indexCount)
            : buildRoomGeometry(&params, style, verts, maxVerts, indices, maxIndices,
                                &vertexCount, &indexCount);
    if (ok) {
        if (ctx->roomVertexBuffer == 0) {
            glGenBuffers(1, &ctx->roomVertexBuffer);
        }
        glBindBuffer(GL_ARRAY_BUFFER, ctx->roomVertexBuffer);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)vertexCount * ROOM_VERTEX_FLOATS * sizeof(float),
                     verts, GL_STATIC_DRAW);
        if (ctx->roomIndexBuffer == 0) {
            glGenBuffers(1, &ctx->roomIndexBuffer);
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->roomIndexBuffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)indexCount * sizeof(unsigned short),
                     indices, GL_STATIC_DRAW);
        // Everything else in here draws from client arrays with no buffer
        // bound, so leaving one bound would turn their pointers into offsets
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        ctx->roomVertexCount = vertexCount;
        ctx->roomIndexCount = indexCount;
        ctx->roomSpillGain = params.spillGain;
        ctx->roomTexMix = params.texMix;
        ctx->roomDim = params.dim;
        if (baked) {
            // A textured room has no wall shade to take this from, and its shell
            // is closed, so all this covers is the frame before the first draw
            ctx->roomClear[0] = 0.010f;
            ctx->roomClear[1] = 0.010f;
            ctx->roomClear[2] = 0.012f;
        }
        else {
            // Darker than any surface in the room, so anything the geometry
            // misses reads as the far end of the same room rather than a hole
            ctx->roomClear[0] = params.wallLevel * 0.5f;
            ctx->roomClear[1] = params.wallLevel * 0.5f;
            ctx->roomClear[2] = params.wallLevel * 0.5f;
        }
        LOGEV("room ready, style %d, scale %.2f, %d vertices, %d indices",
              style, scale, vertexCount, indexCount);
    }
    free(verts);
    free(indices);
    if (!ok) {
        LOGE("room geometry build failed for style %d", style);
    }
    return ok;
}

// Which style can actually be built at this moment. A baked room cannot come up
// until its model and atlas have been read off the assets, so until they land
// the generated room stands in for it and the picker never shows a black world.
static int buildableRoomStyle(XrCtx* ctx, int style) {
    if (style < ROOM_STYLE_MINIMAL) {
        style = ROOM_STYLE_MINIMAL;
    }
    if (style == ROOM_STYLE_PSX && !roomAssetsReady(ctx)) {
        return ROOM_STYLE_MINIMAL;
    }
    return style;
}

// Brings up everything the room draws with, the first frame that asks for it.
// Mid session swapchain creation is already how the background photo arrives.
static int initRoom(XrCtx* ctx) {
    if (ctx->roomReady) {
        return 1;
    }
    if (ctx->roomFailed || ctx->session == XR_NULL_HANDLE) {
        return 0;
    }
    ctx->roomFailed = 1;

    // What the runtime recommends per eye, capped by the chosen tier, so the
    // room's edges are as sharp as the video layer sitting in front of them.
    // That is a couple of hundred megabytes between the side by side colour
    // swapchain and the depth buffer, which is the reason none of it exists
    // until a room is picked. A runtime that will not say what it wants gets a
    // modest guess. Ultra takes a fixed size instead and only asks the runtime
    // for its ceiling, since a recommendation is the one number it is trying to
    // ignore.
    int tier = ctx->envResTier;
    int eyeW;
    int eyeH;
    if (tier == ENV_RES_ULTRA) {
        eyeW = ROOM_ULTRA_EYE;
        eyeH = ROOM_ULTRA_EYE;
        if (ctx->maxEyeWidth > 0 && eyeW > ctx->maxEyeWidth) {
            eyeW = ctx->maxEyeWidth;
        }
        if (ctx->maxEyeHeight > 0 && eyeH > ctx->maxEyeHeight) {
            eyeH = ctx->maxEyeHeight;
        }
    }
    else {
        int maxEye = tier == ENV_RES_LOW ? ROOM_MAX_EYE
                   : tier == ENV_RES_HIGH ? ROOM_MAX_EYE_FULL
                   : ROOM_MAX_EYE_STANDARD;
        eyeW = ctx->recommendedEyeWidth > 0 ? ctx->recommendedEyeWidth : 1024;
        eyeH = ctx->recommendedEyeHeight > 0 ? ctx->recommendedEyeHeight : 1024;
        if (tier == ENV_RES_LOW) {
            eyeW /= 2;
            eyeH /= 2;
        }
        if (eyeW > maxEye) {
            eyeW = maxEye;
        }
        if (eyeH > maxEye) {
            eyeH = maxEye;
        }
    }

    // Side by side, the same arrangement the video swapchain uses in stereo
    if (!createArtSwapchain(ctx, eyeW * ROOM_EYES, eyeH, "create room swapchain",
                            &ctx->roomSwapchain, &ctx->roomImages, &ctx->roomImageCount)) {
        return 0;
    }

    // The one pass in here that needs a depth buffer, since it is the only one
    // drawing geometry that can be in front of other geometry
    glGenRenderbuffers(1, &ctx->roomDepthBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, ctx->roomDepthBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, eyeW * ROOM_EYES, eyeH);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    // Colour comes from whichever swapchain image the frame acquires, so only
    // the depth attachment can be made once
    glGenFramebuffers(1, &ctx->roomFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->roomFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              ctx->roomDepthBuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    GLuint vs = compileShader(GL_VERTEX_SHADER, ROOM_VERTEX_SRC);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, ROOM_FRAGMENT_SRC);
    if (vs == 0 || fs == 0) {
        return 0;
    }
    ctx->roomProgram = glCreateProgram();
    glAttachShader(ctx->roomProgram, vs);
    glAttachShader(ctx->roomProgram, fs);
    glBindAttribLocation(ctx->roomProgram, 0, "a_position");
    glBindAttribLocation(ctx->roomProgram, 1, "a_color");
    glBindAttribLocation(ctx->roomProgram, 2, "a_spill");
    glBindAttribLocation(ctx->roomProgram, 3, "a_uv");
    glLinkProgram(ctx->roomProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(ctx->roomProgram, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(ctx->roomProgram, sizeof(log), NULL, log);
        LOGE("room program link failed: %s", log);
        return 0;
    }
    ctx->roomViewProjUniform = glGetUniformLocation(ctx->roomProgram, "u_viewproj");
    ctx->roomSpillGainUniform = glGetUniformLocation(ctx->roomProgram, "u_spillGain");
    ctx->roomTexMixUniform = glGetUniformLocation(ctx->roomProgram, "u_texMix");
    ctx->roomDimUniform = glGetUniformLocation(ctx->roomProgram, "u_dim");
    glUseProgram(ctx->roomProgram);
    glUniform1i(glGetUniformLocation(ctx->roomProgram, "u_ambi"), 0);
    glUniform1i(glGetUniformLocation(ctx->roomProgram, "u_room"), 1);

    // The atlas sampler is read whatever the mix is set to, so there is always
    // a complete texture on that unit even before an atlas has been loaded
    glGenTextures(1, &ctx->roomWhiteTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->roomWhiteTexture);
    const unsigned char white[4] = { 255, 255, 255, 255 };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Whichever room is being asked for, so the first frame is already the one
    // the picker is on rather than a rebuild later
    int wanted = roomEffective(ctx);
    int style = buildableRoomStyle(ctx, wanted);
    if (!uploadRoomGeometry(ctx, style)) {
        return 0;
    }
    ctx->roomBuiltStyle = style;
    ctx->roomWantedStyle = wanted;
    ctx->roomAssetsSeen = roomAssetsReady(ctx);
    ctx->roomWantedScale = roomScale(ctx, style);

    ctx->roomEyeWidth = eyeW;
    ctx->roomEyeHeight = eyeH;
    ctx->roomReady = 1;
    ctx->roomFailed = 0;
    const char* tierName = tier == ENV_RES_LOW ? "low"
                         : tier == ENV_RES_HIGH ? "high"
                         : tier == ENV_RES_ULTRA ? "ultra"
                         : "standard";
    LOGEV("room ready at %dx%d per eye, env res %s", eyeW, eyeH, tierName);
    return 1;
}

// Where both eyes are this frame. Only the room needs this, so it is only
// asked for while a room is on.
static int locateRoomViews(XrCtx* ctx) {
    XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = ctx->predictedDisplayTime;
    locateInfo.space = ctx->localSpace;

    XrViewState state = { XR_TYPE_VIEW_STATE };
    XrView views[ROOM_EYES];
    for (int eye = 0; eye < ROOM_EYES; eye++) {
        views[eye].type = XR_TYPE_VIEW;
        views[eye].next = NULL;
    }
    uint32_t count = 0;
    if (XR_FAILED(xrLocateViews(ctx->session, &locateInfo, &state, ROOM_EYES, &count, views))
            || count < ROOM_EYES) {
        return 0;
    }
    // Both bits, since a room drawn from an orientation with no position in it
    // would sit still while the head moves through the walls
    XrViewStateFlags needed = XR_VIEW_STATE_ORIENTATION_VALID_BIT
            | XR_VIEW_STATE_POSITION_VALID_BIT;
    if ((state.viewStateFlags & needed) != needed) {
        return 0;
    }

    for (int eye = 0; eye < ROOM_EYES; eye++) {
        ctx->roomViews[eye] = views[eye];
    }
    ctx->roomViewsValid = 1;
    return 1;
}

// Everything the room has to have built or rebuilt before it can be drawn.
// Kept out of the frame's timer query on purpose: a swapchain or a buffer
// created inside that window leaves this driver reporting garbage for every
// sample after it, so all of it happens before the query opens.
void prepareRoom(XrCtx* ctx) {
    if (!initRoom(ctx)) {
        return;
    }
    // The picker can move between rooms with the session running, a baked one
    // can be picked before its assets have arrived, and the scale property can
    // move under either. Nothing about any of them changes frame to frame, so
    // the work only happens when the style asked for, the readiness of those
    // assets or the scale has moved: a build that fails leaves whichever room
    // is already in the buffers and is not tried again.
    int wanted = roomEffective(ctx);
    int assets = roomAssetsReady(ctx);
    int style = buildableRoomStyle(ctx, wanted);
    float scale = roomScale(ctx, style);
    if (wanted == ctx->roomWantedStyle && assets == ctx->roomAssetsSeen
            && scale == ctx->roomWantedScale) {
        return;
    }
    // Decided before the ask is recorded, since the scale is part of both
    int rebuild = style != ctx->roomBuiltStyle || scale != ctx->roomWantedScale;
    ctx->roomWantedStyle = wanted;
    ctx->roomAssetsSeen = assets;
    ctx->roomWantedScale = scale;

    if (rebuild && uploadRoomGeometry(ctx, style)) {
        ctx->roomBuiltStyle = style;
    }
}

// Draws the room into its own image, one half per eye. The layer that shows it
// is submitted in endFrame, with the very poses drawn from here. Nothing is
// created in here: prepareRoom has already been round.
void renderRoom(XrCtx* ctx) {
    if (!ctx->roomReady) {
        return;
    }
    // A frame the eyes could not be located for keeps the image it already
    // has. The layer still goes up, with the poses that image was drawn from.
    if (!locateRoomViews(ctx)) {
        return;
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->roomSwapchain, &acquire, &index),
                 "acquire room image")) {
        return;
    }
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(ctx->roomSwapchain, &wait);

    // Opened only now, with the image in hand: this driver hands back wrapped
    // nonsense for a query that spans the compositor wait above
    int roomTiming = ctx->timerSupported && !ctx->captureRequested
            && !ctx->roomTimerPending[ctx->roomTimerSlot];
    if (roomTiming) {
        pfnBeginQuery(GL_TIME_ELAPSED_EXT, ctx->roomTimerQueries[ctx->roomTimerSlot]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->roomFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->roomImages[index].image, 0);
    // Once, on the first frame drawn. The colour attachment is a swapchain
    // image, so this is the first point the pair of them can be checked, and a
    // room that never appears is otherwise silent.
    if (!ctx->roomRendered) {
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("room framebuffer incomplete: 0x%x", status);
        }
    }
    // The colours below are authored the way the video arrives, already gamma
    // encoded, so the write must not encode them a second time
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glClearColor(ctx->roomClear[0], ctx->roomClear[1], ctx->roomClear[2], 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    glUseProgram(ctx->roomProgram);
    // The atlas a baked room is painted with, or the white stand in, which the
    // mix below leaves out of the picture anyway
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx->roomTextureReady ? ctx->roomTexture
                                                       : ctx->roomWhiteTexture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->ambiTexture);
    // Nothing has been sampled off the video yet on the first frames, so the
    // room is just its baked self until there is, and the same for the option
    // turned off: the baked colours and the atlas stay, only the light the
    // picture throws goes. Deliberately not tied to the ambilight: the wash
    // inside a room and the glow around a floating screen are different
    // effects, and the colour sample they share is taken for either one.
    int lit = ctx->ambiSeeded && ctx->roomLightOn;
    glUniform1f(ctx->roomSpillGainUniform, lit ? ctx->roomSpillGain : 0.0f);
    glUniform1f(ctx->roomTexMixUniform, ctx->roomTexMix);
    glUniform1f(ctx->roomDimUniform, roomDim(ctx));

    glBindBuffer(GL_ARRAY_BUFFER, ctx->roomVertexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->roomIndexBuffer);
    GLsizei stride = ROOM_VERTEX_FLOATS * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (const void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (const void*)(7 * sizeof(float)));
    glEnableVertexAttribArray(3);

    for (int eye = 0; eye < ROOM_EYES; eye++) {
        glViewport(eye * ctx->roomEyeWidth, 0, ctx->roomEyeWidth, ctx->roomEyeHeight);

        float proj[16];
        float view[16];
        float viewProj[16];
        // Near enough to walk into a wall without it clipping, far enough to
        // hold a room a few metres across
        projectionFromFov(proj, ctx->roomViews[eye].fov, 0.05f, 60.0f);
        viewFromPose(view, ctx->roomViews[eye].pose);
        matMul(viewProj, proj, view);
        glUniformMatrix4fv(ctx->roomViewProjUniform, 1, GL_FALSE, viewProj);

        glDrawElements(GL_TRIANGLES, ctx->roomIndexCount, GL_UNSIGNED_SHORT, (const void*)0);
    }

    if (roomTiming) {
        pfnEndQuery(GL_TIME_ELAPSED_EXT);
        ctx->roomTimerPending[ctx->roomTimerSlot] = 1;
        ctx->roomTimerPendingFrames[ctx->roomTimerSlot] = 0;
        ctx->roomTimerSlot = 1 - ctx->roomTimerSlot;
    }

    glDisable(GL_DEPTH_TEST);
    // Handed back exactly as the other passes expect to find it: no buffers
    // bound, since they all draw from client arrays, and only the two attribute
    // arrays they use left on
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->roomSwapchain, &release);
    ctx->roomRendered = 1;
}

// The baked room model. Read off the assets in Java and parsed here, since the
// renderer has no glTF loader: the bake script has already flattened it to
// positions, normals and texture coordinates. Handed over from the frame loop,
// which is the thread that builds the geometry out of it.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadRoomModel(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jobject buffer,
                                                                   jint length) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || buffer == NULL || length < 12) {
        return;
    }
    const unsigned char* data = (const unsigned char*)(*env)->GetDirectBufferAddress(env, buffer);
    if (data == NULL || (*env)->GetDirectBufferCapacity(env, buffer) < (jlong)length) {
        return;
    }
    if (memcmp(data, "MXR1", 4) != 0) {
        LOGW("room model is not an MXR1 file, ignoring it");
        return;
    }

    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    memcpy(&vertexCount, data + 4, sizeof(vertexCount));
    memcpy(&indexCount, data + 8, sizeof(indexCount));
    // Both are held to what the file could possibly hold before any of the byte
    // counts are worked out, so none of the arithmetic below can wrap
    size_t payload = (size_t)length - 12;
    if (vertexCount == 0 || vertexCount > ROOM_MAX_VERTS
            || indexCount == 0 || indexCount % 3 != 0
            || indexCount > payload / sizeof(unsigned short)) {
        LOGW("room model counts make no sense: %u vertices, %u indices",
             vertexCount, indexCount);
        return;
    }
    size_t vertexBytes = (size_t)vertexCount * ROOM_MODEL_FLOATS * sizeof(float);
    size_t indexBytes = (size_t)indexCount * sizeof(unsigned short);
    if (12 + vertexBytes + indexBytes != (size_t)length) {
        LOGW("room model is %d bytes, its header asks for %zu",
             length, 12 + vertexBytes + indexBytes);
        return;
    }

    float* verts = malloc(vertexBytes);
    unsigned short* indices = malloc(indexBytes);
    if (verts == NULL || indices == NULL) {
        free(verts);
        free(indices);
        LOGE("room model allocation failed");
        return;
    }
    memcpy(verts, data + 12, vertexBytes);
    memcpy(indices, data + 12 + vertexBytes, indexBytes);

    for (uint32_t i = 0; i < indexCount; i++) {
        if (indices[i] >= vertexCount) {
            free(verts);
            free(indices);
            LOGW("room model index %u is past its %u vertices",
                 (unsigned)indices[i], vertexCount);
            return;
        }
    }
    // Kept in the model's own space. The anchor and the scale go on as the
    // geometry is built, so the scale can move without this being read again.
    free(ctx->roomModelVerts);
    free(ctx->roomModelIndices);
    ctx->roomModelVerts = verts;
    ctx->roomModelIndices = indices;
    ctx->roomModelVertexCount = (int)vertexCount;
    ctx->roomModelIndexCount = (int)indexCount;
    ctx->roomModelReady = 1;
    LOGEV("room model ready, %u vertices, %u indices", vertexCount, indexCount);
}

// The atlas that model is painted with. A plain texture rather than a swapchain,
// since nothing composites it: the room samples it as it draws. Also from the
// frame loop, which is where the GL context is current.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadRoomTexture(JNIEnv* env, jobject thiz,
                                                                     jlong handle, jobject buffer,
                                                                     jint width, jint height) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || buffer == NULL || width <= 0 || height <= 0) {
        return;
    }
    const unsigned char* px = (const unsigned char*)(*env)->GetDirectBufferAddress(env, buffer);
    if (px == NULL
            || (*env)->GetDirectBufferCapacity(env, buffer) < (jlong)width * height * 4) {
        return;
    }

    if (ctx->roomTexture == 0) {
        glGenTextures(1, &ctx->roomTexture);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->roomTexture);
    // The rows arrive top down out of the decoder and the model's texture
    // coordinates start at the top too, so this one is not flipped on the way in
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, px);
    // A wall seen at a glancing angle across a room this size is minified hard,
    // so the atlas is worth the mip chain
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    ctx->roomTextureReady = 1;
    LOGEV("room texture %dx%d ready", width, height);
}
