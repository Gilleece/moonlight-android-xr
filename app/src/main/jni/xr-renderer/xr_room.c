// The 3d rooms: baked models, drawn per eye into the one projection layer
// this renderer has, with the picture hung on the far wall.
#include "xr_renderer.h"
#include "xr_shaders.h"
#include "xr_atlas.h"
#include "xr_roommesh.h"

// Everything a room's shape and lighting is made of, gathered in one place so
// the look can be changed without reading the model
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
    // Where the picture hangs, which is what the light is baked from
    Vec3 screenAt;
    // The point in the model's own space that lands on the viewer's origin.
    // The geometry is built as (model - anchor) * scale.
    Vec3 anchor;
    // How high on the wall the picture is mounted, and how far off the wall it
    // stands so the two never fight for the same pixels
    float screenMountY;
    float screenProud;
    // How wide and how tall the room's screen anchor is, which is the largest
    // the picture is hung. The room sizes its own picture rather than taking
    // the size slider's, since the wall it goes on is a known size, and a
    // picture of another shape fits inside the two rather than running off it.
    float screenWidth;
    float screenHeight;
    // Whether the picture may hang past the walls, the floor and the ceiling,
    // which is the row's own flag
    int open;
    // Distance at which the screen's light is down to half
    float spillRadius;
    // How much of that light a fully lit vertex takes
    float spillGain;
    // How much of a textured part's colour comes off its atlas, and how far
    // down the whole room is turned on the way in
    float texMix;
    float dim;
    unsigned seed;
} RoomParams;

// A dither of about one 255th, from the seed and the vertex number. Without it
// the gradients the light lays over a wall are shallow enough over enough
// pixels to band.
static float roomDither(unsigned seed, unsigned index) {
    unsigned h = seed + index * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return ((float)(h & 0xffffu) / 65535.0f - 0.5f) * (2.0f / 255.0f);
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

// Writes one vertex in the layout the room's buffer is in
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

// Which room is in force, the picker's unless the debug property has taken it
// over. 0 is no room at all, which is every other environment.
int roomEffective(XrCtx* ctx) {
    return ctx->roomOverride >= 0 ? ctx->roomOverride : ctx->roomStyle;
}

// One room as it was measured off its model, in the model's own space and in
// metres. Every room ships this way: modelled around a Viewer_Seated node, with
// a ScreenAnchor empty giving the centre of the picture and the largest it may
// be hung, so a room is a row here and nothing else. Every number goes through
// the same (model - anchor) * scale as the geometry.
typedef struct {
    // The Viewer_Seated node the model carries, and where the viewer ends up
    // relative to it: up, and in toward the screen. Set by eye in a headset,
    // where the marked points sat too low and too far back to watch from.
    Vec3 eye;
    float eyeRaise;
    float eyeForward;
    // The ScreenAnchor's origin and its scale, which is the picture's largest
    // width and height. The room was lit and seated for that size.
    Vec3 screen;
    float screenWidth;
    float screenHeight;
    // How much of the anchor the picture hangs on to start with, a fraction of
    // its width with the picture's own shape kept
    float screenFraction;
    // The floor the architecture stands on, the ceiling over it, the side walls
    // and the wall behind the viewer
    float floorY;
    float ceilingY;
    float halfWidth;
    float backZ;
    // How far the picture's light carries, and how much of it a fully lit
    // vertex takes
    float spillRadius;
    float spillGain;
    // How bright the room starts, a factor over its atlases and colours as
    // they were baked. The Room tab's brightness row moves it from there.
    float dim;
    // The dither seed, which only has to differ room to room
    unsigned seed;
    // The size the room is drawn at, which is the size it was built
    float scale;
    // Whether the picture may be resized inside the anchor. A room is lit and
    // seated for the picture filling it, so that is only allowed where the
    // picture hangs in a space rather than on a wall built around it.
    int resizable;
    // Whether the picture may hang past the room's own walls, floor and
    // ceiling. A room built around its picture is clamped to them, so an anchor
    // wider than the wall cannot cut through it; a room in the open has
    // nothing behind the picture but sky.
    int open;
    // Whether the glow around the picture starts on in this room. A dark room
    // is spoiled by it.
    int glow;
} RoomModel;

// Every room that ships, indexed by its style less ROOM_STYLE_FIRST. The
// values the Room tab starts from are in xr_shared.h, where the preferences
// read their defaults off the same numbers.
static const RoomModel ROOM_MODELS[] = {
    // Home Theater: a small room with the picture flat on the front wall, four
    // fifths of the anchor, 2.88 m across, which sits better from the seat than
    // a picture filling the whole of it. The light is down to half about the
    // depth of the seating, and over a painted atlas less gain than this never
    // reads as light at all. A little over a third of the atlas as it was
    // baked, picked by eye in a headset.
    {
        .eye = { 0.0f, 1.15f, 1.12f }, .eyeRaise = 0.35f, .eyeForward = 0.10f,
        .screen = { 0.0f, 1.55f, -3.132f }, .screenWidth = 3.6f, .screenHeight = 2.025f,
        .screenFraction = ROOM_THEATER_SCREEN / 100.0f,
        .floorY = -0.14f, .ceilingY = 2.84f, .halfWidth = 2.8f, .backZ = 4.41f,
        .spillRadius = 3.0f, .spillGain = 0.55f, .dim = ROOM_THEATER_BRIGHTNESS / 100.0f,
        .seed = 0xc2b2ae35u, .scale = 1.0f, .resizable = ROOM_THEATER_RESIZABLE, .open = 0,
        .glow = ROOM_THEATER_GLOW,
    },
    // Grand Cinema: a raked auditorium, watched from partway up the rake, with
    // the picture filling the whole of its screen. Built around that picture,
    // so it cannot be resized, and dark enough that the glow spoils it.
    {
        .eye = { 0.0f, 4.8f, 6.0f }, .eyeRaise = 0.40f, .eyeForward = 0.0f,
        .screen = { 0.0f, 7.4f, -15.42f }, .screenWidth = 22.0f, .screenHeight = 12.375f,
        .screenFraction = ROOM_GRAND_CINEMA_SCREEN / 100.0f,
        .floorY = -0.3f, .ceilingY = 16.3f, .halfWidth = 14.3f, .backZ = 16.3f,
        .spillRadius = 8.0f, .spillGain = 0.55f, .dim = ROOM_GRAND_CINEMA_BRIGHTNESS / 100.0f,
        .seed = 0x85ebca6bu, .scale = 1.0f, .resizable = ROOM_GRAND_CINEMA_RESIZABLE,
        .open = 0, .glow = ROOM_GRAND_CINEMA_GLOW,
    },
    // Synthwave: no room at all, a ground and a sky that run to the horizon,
    // so the walls are put where nothing can reach them and the picture hangs
    // in the open. 14 m across and raised to y 4.5, which keeps its bottom edge
    // half a metre off the ground; the model's own anchor says 10 by 5.625 at
    // y 3.1. The light falls off slowly and little of it comes back.
    {
        .eye = { 0.0f, 1.2f, 0.0f }, .eyeRaise = 0.35f, .eyeForward = 0.10f,
        .screen = { 0.0f, 4.5f, -14.0f }, .screenWidth = 14.0f, .screenHeight = 7.875f,
        .screenFraction = ROOM_SYNTHWAVE_SCREEN / 100.0f,
        .floorY = 0.0f, .ceilingY = 1800.0f, .halfWidth = 1800.0f, .backZ = 1800.0f,
        .spillRadius = 6.0f, .spillGain = 0.4f, .dim = ROOM_SYNTHWAVE_BRIGHTNESS / 100.0f,
        .seed = 0x2545f491u, .scale = 1.0f, .resizable = ROOM_SYNTHWAVE_RESIZABLE,
        .open = 1, .glow = ROOM_SYNTHWAVE_GLOW,
    },
};

// A row a style, so the table and the list of styles cannot drift apart
_Static_assert(sizeof(ROOM_MODELS) / sizeof(ROOM_MODELS[0])
               == ROOM_STYLE_LAST - ROOM_STYLE_FIRST + 1, "a room style with no row");

// Whether a style is one of the rooms that ship as a model
static int bakedRoomStyle(int style) {
    return style >= ROOM_STYLE_FIRST && style <= ROOM_STYLE_LAST;
}

// The row a style reads. Anything unknown falls back to the first room rather
// than reading past the table.
static const RoomModel* roomModel(int style) {
    if (!bakedRoomStyle(style)) {
        return &ROOM_MODELS[0];
    }
    return &ROOM_MODELS[style - ROOM_STYLE_FIRST];
}

// Which room a style asks for, at the scale that style is drawn. The anchor is
// the model's seated eye point moved by the two offsets, and every number off
// the model goes through the same (model - anchor) * scale the geometry does,
// so moving the anchor moves the whole room around the viewer.
static RoomParams roomParams(int style, float scale) {
    const RoomModel* m = roomModel(style);

    RoomParams p;
    memset(&p, 0, sizeof(p));
    Vec3 anchor = { m->eye.x, m->eye.y + m->eyeRaise, m->eye.z - m->eyeForward };
    p.anchor = anchor;
    p.halfWidth = m->halfWidth * scale;
    p.floorY = (m->floorY - anchor.y) * scale;
    // One floor throughout in every room that ships, so the picture stands on
    // the same one the viewer does
    p.screenFloorY = p.floorY;
    p.ceilingY = (m->ceilingY - anchor.y) * scale;
    p.screenZ = (m->screen.z - anchor.z) * scale;
    p.backZ = (m->backZ - anchor.z) * scale;
    p.screenMountY = (m->screen.y - anchor.y) * scale;
    // The picture sits flat against the anchor, so nothing stands it off
    p.screenProud = 0.0f;
    p.screenWidth = m->screenWidth * scale;
    p.screenHeight = m->screenHeight * scale;
    p.open = m->open;
    Vec3 screenAt = { (m->screen.x - anchor.x) * scale, p.screenMountY, p.screenZ };
    p.screenAt = screenAt;
    p.spillRadius = m->spillRadius * scale;
    p.spillGain = m->spillGain;
    p.texMix = 1.0f;
    p.dim = m->dim;
    p.seed = m->seed;
    return p;
}

// Whether the picture may be resized in a style's room. Only a room answers
// here: outside one the picture is the user's to size however they like.
int roomResizable(int style) {
    return bakedRoomStyle(style) && roomModel(style)->resizable;
}

// Every room's own values as its row starts them, which is where they stay
// until the preferences are handed down. The light level starts at the same
// place in every room.
void roomLevelsFromTable(XrCtx* ctx) {
    for (int style = ROOM_STYLE_FIRST; style <= ROOM_STYLE_LAST; style++) {
        const RoomModel* m = roomModel(style);
        ctx->roomBrightness[style] = (int)roundf(m->dim * 100.0f);
        ctx->roomGlow[style] = m->glow != 0;
        ctx->roomLightLevel[style] = ROOM_LIGHT_DEFAULT;
        ctx->roomScreen[style] = (int)roundf(m->screenFraction * 100.0f);
    }
}

// How much of its anchor a style's room hangs the picture on, in whole
// percent. A room that cannot be resized hangs the whole of it whatever was
// saved for it.
int roomScreenPercent(XrCtx* ctx, int style) {
    if (!bakedRoomStyle(style)) {
        return ROOM_SCREEN_MAX;
    }
    return roomScreenClamp(ctx->roomScreen[style], roomResizable(style));
}

// Whether the glow goes up around the picture. In a room that is the room's
// own switch, and anywhere else the one app wide switch.
int roomGlowOn(XrCtx* ctx, int style) {
    return bakedRoomStyle(style) ? ctx->roomGlow[style] : ctx->ambilightOn;
}

// Whether the picture's size is under a hand or a thumb right now, which is
// when its every step would otherwise be a line in the log
static int roomSizeDragging(XrCtx* ctx) {
    return ctx->grabMode == GRAB_RESIZE
            || (ctx->cogDragSlider == COG_ROOM_ROW_SIZE && ctx->cogDragFace == COG_FACE_ROOM);
}

// How large a style is drawn, which is the size it was built at unless a
// property set inside the range says otherwise
static float roomScale(XrCtx* ctx, int style) {
    if (!bakedRoomStyle(style)) {
        return 1.0f;
    }
    float scale = ctx->roomScaleOverride > 0.0f ? ctx->roomScaleOverride
                                                : roomModel(style)->scale;
    if (scale < ROOM_SCALE_MIN) {
        scale = ROOM_SCALE_MIN;
    }
    if (scale > ROOM_SCALE_MAX) {
        scale = ROOM_SCALE_MAX;
    }
    return scale;
}

// How far the room's own pass has to see. Every room is drawn on its own, so
// the far plane is that room's rather than one number that has to suit all of
// them: the geometry's own reach with room to spare, the walls the table gives
// under that in case a model comes up short, and what every room was drawn with
// before as the floor, so a small room keeps the precision it had and a sky that
// runs to the horizon is inside the frustum.
static float roomFarPlane(const RoomParams* p, float reach) {
    float walls = p->halfWidth;
    if (fabsf(p->backZ) > walls) {
        walls = fabsf(p->backZ);
    }
    if (p->ceilingY > walls) {
        walls = p->ceilingY;
    }
    if (reach > walls) {
        walls = reach;
    }
    float far = walls * ROOM_FAR_MARGIN;
    return far < ROOM_FAR_MIN_M ? ROOM_FAR_MIN_M : far;
}

// How far down the room is turned as it draws, which is the room's own
// brightness row. Nothing is baked into the geometry from this, so the row and
// the property move it frame to frame with no rebuild behind them, and the
// property wins over the row.
static float roomDim(XrCtx* ctx) {
    if (ctx->roomDimOverride <= 0.0f) {
        int style = ctx->roomBuiltStyle;
        return bakedRoomStyle(style) ? ctx->roomBrightness[style] / 100.0f : ctx->roomDim;
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
        // A room's corner still held as the room went would carry on as a free
        // resize of the placement just handed back
        ctx->grabMode = GRAB_NONE;
        if (ctx->recentredInRoom) {
            ctx->recentredInRoom = 0;
            ctx->poseDirty = 1;
        }
    }
    if (!roomOn) {
        ctx->roomPlacedStyle = 0;
        return;
    }

    // The same scale the geometry was built at, so the picture and the walls
    // around it never disagree
    RoomParams p = roomParams(style, roomScale(ctx, style));
    // The room says how big its picture is, not the size slider: the anchor is
    // a known size and the picture is fitted inside it with its own shape
    // kept, so a taller film loses width rather than running up the wall, and
    // hung at the share of that the room's size row says
    float width = p.screenWidth;
    if (aspect > 0.0f && p.screenHeight > 0.0f && width * aspect > p.screenHeight) {
        width = p.screenHeight / aspect;
    }
    width *= roomScreenPercent(ctx, style) / 100.0f;
    // The clamps only catch a room whose own anchor does not fit its wall, and
    // an open room has no wall to fit: its anchor is hung as it is written
    if (!p.open) {
        float maxWidth = 2.0f * p.halfWidth - 0.4f;
        float maxHeight = (p.ceilingY - p.floorY) - 0.3f;
        if (width > maxWidth) {
            width = maxWidth;
        }
        if (width * aspect > maxHeight) {
            width = maxHeight / aspect;
        }
    }
    float height = width * aspect;
    // And hung where the whole of it is on the wall rather than through the
    // floor or the ceiling. The floor here is the one under the picture, not
    // the tier the viewer is on, which in a raked room is metres higher and
    // would push the picture back up the wall. An open room is left alone: its
    // picture is meant to stand above the room.
    float mount = p.screenMountY;
    if (!p.open) {
        float lowest = p.screenFloorY + height * 0.5f + 0.1f;
        float highest = p.ceilingY - height * 0.5f - 0.1f;
        if (mount < lowest) {
            mount = lowest;
        }
        if (mount > highest) {
            mount = highest;
        }
    }

    // Square to the wall and facing the viewer, the same identity orientation
    // the placement starts out with
    memset(&ctx->screenPose, 0, sizeof(ctx->screenPose));
    ctx->screenPose.orientation.w = 1.0f;
    ctx->screenPose.position.x = p.screenAt.x;
    ctx->screenPose.position.y = mount;
    ctx->screenPose.position.z = p.screenZ + p.screenProud;
    ctx->screenWidth = width;
    // Once per room and picture shape rather than every frame, and once for a
    // resize, when it lets go, rather than for every step of it
    if (style != ctx->roomPlacedStyle
            || (fabsf(width - ctx->roomPlacedWidth) > 0.001f && !roomSizeDragging(ctx))) {
        ctx->roomPlacedStyle = style;
        ctx->roomPlacedWidth = width;
        LOGEV("room %d hangs the picture %.2f by %.2f m, centre y %.2f z %.2f",
              style, width, height, mount, ctx->screenPose.position.z);
    }
}

// Whether everything a baked room is made of has arrived and belongs to the
// room being asked about: the model, and every atlas the model asks for. One
// room is resident, so a style whose turn it is waits here while its own set is
// read.
static int roomAssetsReady(XrCtx* ctx, int style) {
    if (!ctx->roomModelReady || ctx->roomModelStyle != style) {
        return 0;
    }
    for (int i = 0; i < ctx->roomAtlasCount; i++) {
        if (!ctx->roomTextureReady[i] || ctx->roomTextureStyle[i] != style) {
            return 0;
        }
    }
    return 1;
}

// Turns the loaded model into the layout the room's buffer is in. Nothing is
// generated here beyond the light: the shape, the texture coordinates and the
// colour all come off the model. A textured part carries no colour of its own
// and its atlas is mixed over the top of it as the part draws; a part painted
// from its vertex colours carries them here. The model arrives in its own
// space, so this is where the anchor and the scale go on. The normals are left
// alone, since a uniform scale does not turn them. Hands back how far the room
// runs from the viewer, which is what its far plane is worked out from, taken
// here because the vertices are being walked anyway.
static float buildModelRoomVertices(XrCtx* ctx, const RoomParams* p, float scale,
                                    float* verts) {
    float furthest = 0.0f;
    for (int i = 0; i < ctx->roomModelVertexCount; i++) {
        const float* src = ctx->roomModelVerts + (size_t)i * ROOM_MODEL_FLOATS;
        Vec3 pos = { (src[0] - p->anchor.x) * scale,
                     (src[1] - p->anchor.y) * scale,
                     (src[2] - p->anchor.z) * scale };
        float away = pos.x * pos.x + pos.y * pos.y + pos.z * pos.z;
        if (away > furthest) {
            furthest = away;
        }
        Vec3 normal = { src[3], src[4], src[5] };
        roomWriteVertex(p, verts, i, pos, src + 8, roomSpillWeight(p, pos, normal),
                        src[6], src[7]);
    }
    return sqrtf(furthest);
}

// Builds a style's room and hands it to the buffers. Called once for the first
// room and again whenever the picker moves to another: a one off pass over a
// few thousand vertices, which is cheaper than keeping every room resident for
// a switch that may never come.
static int uploadRoomGeometry(XrCtx* ctx, int style) {
    // No room, which is what a room whose assets have not arrived asks for. The
    // buffers empty and the layer clears black until they do.
    if (style <= 0) {
        ctx->roomVertexCount = 0;
        ctx->roomIndexCount = 0;
        ctx->roomPartCount = 0;
        ctx->roomFarZ = ROOM_FAR_MIN_M;
        ctx->roomClear[0] = 0.0f;
        ctx->roomClear[1] = 0.0f;
        ctx->roomClear[2] = 0.0f;
        return 1;
    }
    if (!bakedRoomStyle(style) || !roomAssetsReady(ctx, style)) {
        return 0;
    }
    float scale = roomScale(ctx, style);
    RoomParams params = roomParams(style, scale);
    int vertexCount = ctx->roomModelVertexCount;
    int indexCount = ctx->roomModelIndexCount;
    float* verts = malloc((size_t)vertexCount * ROOM_VERTEX_FLOATS * sizeof(float));
    if (verts == NULL) {
        LOGE("room geometry allocation failed");
        return 0;
    }
    float reach = buildModelRoomVertices(ctx, &params, scale, verts);
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
    // The model's own indices, since the parts draw its vertices as they came
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)indexCount * sizeof(uint32_t),
                 ctx->roomModelIndices, GL_STATIC_DRAW);
    // Everything else in here draws from client arrays with no buffer bound,
    // so leaving one bound would turn their pointers into offsets
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    free(verts);

    ctx->roomVertexCount = vertexCount;
    ctx->roomIndexCount = indexCount;
    memcpy(ctx->roomParts, ctx->roomModelParts,
           (size_t)ctx->roomModelPartCount * sizeof(RoomMeshPart));
    ctx->roomPartCount = ctx->roomModelPartCount;
    ctx->roomFarZ = roomFarPlane(&params, reach);
    ctx->roomSpillGain = params.spillGain;
    ctx->roomTexMix = params.texMix;
    ctx->roomDim = params.dim;
    // A textured room has no wall shade to take this from, and its shell is
    // closed, so all this covers is the frame before the first draw
    ctx->roomClear[0] = 0.010f;
    ctx->roomClear[1] = 0.010f;
    ctx->roomClear[2] = 0.012f;
    LOGEV("room ready, style %d, scale %.2f, %d vertices, %d indices, %d parts, far %.0f m, "
          "open %d, resizable %d, screen %d percent of its anchor, glow %s, brightness %d, "
          "light level %d", style, scale, vertexCount, indexCount, ctx->roomPartCount,
          (double)ctx->roomFarZ, params.open, roomResizable(style),
          roomScreenPercent(ctx, style), ctx->roomGlow[style] ? "on" : "off",
          ctx->roomBrightness[style], ctx->roomLightLevel[style]);
    return 1;
}

// Which style can actually be built at this moment. A room cannot come up until
// its model and atlas have been read off the assets, so until they land there is
// nothing to draw and 0 comes back: the layer clears black and the picture hangs
// where the room will put it, which is a moment of void rather than a wrong room.
static int buildableRoomStyle(XrCtx* ctx, int style) {
    return roomAssetsReady(ctx, style) ? style : 0;
}

// Brings up everything the room draws with, the first frame that asks for it.
// A swapchain made mid session, the way the panels' art arrives.
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
    ctx->roomAssetsSeen = roomAssetsReady(ctx, wanted);
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
    // The wanted room's own, so a pair landing for one room is not taken for
    // the pair another is still waiting on
    int assets = roomAssetsReady(ctx, wanted);
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

// The room itself, once per eye into its half of the image. Only called with
// geometry in the buffers, from the pass below, which has the framebuffer bound
// and cleared.
static void drawRoomEyes(XrCtx* ctx) {
    glUseProgram(ctx->roomProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->ambiTexture);
    // Nothing has been sampled off the video yet on the first frames, so the
    // room is just its baked self until there is, and the same for the option
    // turned off: the baked colours and the atlas stay, only the light the
    // picture throws goes. Deliberately not tied to the ambilight: the wash
    // inside a room and the glow around a floating screen are different
    // effects, and the colour sample they share is taken for either one. How
    // much of the room's gain goes on is the room's own light level.
    int lit = ctx->ambiSeeded && ctx->roomLightOn;
    int built = ctx->roomBuiltStyle;
    float level = bakedRoomStyle(built) ? ctx->roomLightLevel[built] / 100.0f : 1.0f;
    glUniform1f(ctx->roomSpillGainUniform, lit ? ctx->roomSpillGain * level : 0.0f);
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

    // The unit the parts bind their own atlas to as they draw
    glActiveTexture(GL_TEXTURE1);
    for (int eye = 0; eye < ROOM_EYES; eye++) {
        glViewport(eye * ctx->roomEyeWidth, 0, ctx->roomEyeWidth, ctx->roomEyeHeight);

        float proj[16];
        float view[16];
        float viewProj[16];
        // Near enough to walk into a wall without it clipping, far enough to
        // hold whatever this room reaches to
        projectionFromFov(proj, ctx->roomViews[eye].fov, ROOM_NEAR_M, ctx->roomFarZ);
        viewFromPose(view, ctx->roomViews[eye].pose);
        matMul(viewProj, proj, view);
        glUniformMatrix4fv(ctx->roomViewProjUniform, 1, GL_FALSE, viewProj);

        // A part at a time, each from its own atlas or its own colours. A part
        // with no atlas is painted from the vertex colours the model came with,
        // so the mix goes the other way for it. Only ever an atlas of the room
        // in the buffers, so no frame can paint one room with another's, and
        // the white stand in only ever covers a room that lost one.
        for (int i = 0; i < ctx->roomPartCount; i++) {
            const RoomMeshPart* part = &ctx->roomParts[i];
            int textured = part->atlas >= 0;
            GLuint texture = ctx->roomWhiteTexture;
            if (textured && ctx->roomTextureReady[part->atlas]
                    && ctx->roomTextureStyle[part->atlas] == ctx->roomBuiltStyle) {
                texture = ctx->roomTextures[part->atlas];
            }
            glBindTexture(GL_TEXTURE_2D, texture);
            glUniform1f(ctx->roomTexMixUniform, textured ? ctx->roomTexMix : 0.0f);
            glDrawElements(GL_TRIANGLES, (GLsizei)part->indexCount, GL_UNSIGNED_INT,
                           (const void*)((size_t)part->firstIndex * sizeof(uint32_t)));
        }
    }
    // Every other pass leaves unit 0 the active one
    glActiveTexture(GL_TEXTURE0);
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

    // Nothing until the room's model and atlas have both landed: the clear
    // above is the whole pass until they do
    if (ctx->roomIndexCount > 0) {
        drawRoomEyes(ctx);
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

// One atlas slot, dropped. Every upload makes a fresh texture, so nothing of
// the one before, a compressed chain or its level count, can carry over onto
// the next.
static void releaseRoomTexture(XrCtx* ctx, int slot) {
    if (ctx->roomTextures[slot] != 0) {
        glDeleteTextures(1, &ctx->roomTextures[slot]);
        ctx->roomTextures[slot] = 0;
    }
    ctx->roomTextureReady[slot] = 0;
    ctx->roomTextureStyle[slot] = 0;
}

// Every atlas the resident room brought with it, so a room that is going gives
// its memory back before the room arriving asks for its own
static void releaseRoomTextures(XrCtx* ctx) {
    for (int slot = 0; slot < ROOM_MESH_ATLASES_MAX; slot++) {
        releaseRoomTexture(ctx, slot);
    }
}

// The baked room a picker cell names, or 0 with a line in the log for a cell
// that has no model behind it
static int bakedStyleForCell(int cell, const char* what) {
    int style = roomStyleForCell(cell);
    if (!bakedRoomStyle(style)) {
        LOGW("room %s for cell %d, which is not a baked room, ignoring it", what, cell);
        return 0;
    }
    return style;
}

// A baked room's model, and the cell whose room it is. Read off the assets in
// Java and parsed here, since the renderer has no glTF loader: the bake script
// has already flattened it to positions, normals, texture coordinates and
// colours, and cut it into the parts the draw walks. Handed over from the frame
// loop, which is the thread that builds the geometry out of it.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadRoomModel(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jobject buffer,
                                                                   jint length, jint cell) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || buffer == NULL || length <= 0) {
        return;
    }
    int style = bakedStyleForCell(cell, "model");
    if (style == 0) {
        return;
    }
    const unsigned char* data = (const unsigned char*)(*env)->GetDirectBufferAddress(env, buffer);
    if (data == NULL || (*env)->GetDirectBufferCapacity(env, buffer) < (jlong)length) {
        return;
    }
    RoomMeshInfo info;
    if (!roomMeshParse(data, (size_t)length, &info)) {
        LOGW("room model for style %d is not a mesh the renderer reads (%d bytes)", style, length);
        return;
    }

    size_t vertexBytes = (size_t)info.vertexCount * ROOM_MODEL_FLOATS * sizeof(float);
    size_t indexBytes = (size_t)info.indexCount * sizeof(uint32_t);
    float* verts = malloc(vertexBytes);
    uint32_t* indices = malloc(indexBytes);
    if (verts == NULL || indices == NULL) {
        free(verts);
        free(indices);
        LOGE("room model allocation failed");
        return;
    }
    memcpy(verts, data + info.vertexOffset, vertexBytes);
    memcpy(indices, data + info.indexOffset, indexBytes);

    // The room that was resident is going, so its atlases go with it rather
    // than waiting in GL memory to be painted on this one
    if (ctx->roomModelStyle != style) {
        releaseRoomTextures(ctx);
    }
    // Kept in the model's own space. The anchor and the scale go on as the
    // geometry is built, so the scale can move without this being read again.
    free(ctx->roomModelVerts);
    free(ctx->roomModelIndices);
    ctx->roomModelVerts = verts;
    ctx->roomModelIndices = indices;
    ctx->roomModelVertexCount = (int)info.vertexCount;
    ctx->roomModelIndexCount = (int)info.indexCount;
    memcpy(ctx->roomModelParts, info.parts, (size_t)info.partCount * sizeof(RoomMeshPart));
    ctx->roomModelPartCount = (int)info.partCount;
    ctx->roomAtlasCount = (int)info.atlasCount;
    ctx->roomModelStyle = style;
    ctx->roomModelReady = 1;
    LOGEV("room model ready, style %d, %u vertices, %u triangles, %u parts, %d painted from "
          "vertex colours, %u atlases", style, info.vertexCount, info.indexCount / 3,
          info.partCount, roomMeshPaintedParts(&info), info.atlasCount);
}

// One room's own values for the Room tab's rows, as its preferences keep them,
// by the picker cell that shows it. Handed down for every room when the session
// starts, so the picker can move between them without asking again. Anything
// out of its lane is brought back inside it.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetRoomLevels(JNIEnv* env, jobject thiz,
                                                                jlong handle, jint cell,
                                                                jint brightness, jboolean glow,
                                                                jint light, jint screen) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    int style = bakedStyleForCell(cell, "levels");
    if (style == 0) {
        return;
    }
    ctx->roomBrightness[style] = brightness < ROOM_BRIGHTNESS_MIN ? ROOM_BRIGHTNESS_MIN
            : (brightness > ROOM_BRIGHTNESS_MAX ? ROOM_BRIGHTNESS_MAX : brightness);
    ctx->roomGlow[style] = glow ? 1 : 0;
    ctx->roomLightLevel[style] = light < ROOM_LIGHT_MIN ? ROOM_LIGHT_MIN
            : (light > ROOM_LIGHT_MAX ? ROOM_LIGHT_MAX : light);
    ctx->roomScreen[style] = roomScreenClamp(screen, roomResizable(style));
    LOGEV("room %d levels: brightness %d, glow %s, light level %d, screen %d percent",
          style, ctx->roomBrightness[style], ctx->roomGlow[style] ? "on" : "off",
          ctx->roomLightLevel[style], ctx->roomScreen[style]);
}

// The GL format for a block size the atlas tool writes
static GLenum roomAtlasFormat(uint32_t block) {
    if (block == 4) {
        return GL_COMPRESSED_RGBA_ASTC_4x4_KHR;
    }
    if (block == 8) {
        return GL_COMPRESSED_RGBA_ASTC_8x8_KHR;
    }
    return GL_COMPRESSED_RGBA_ASTC_6x6_KHR;
}

// One of a room's atlases, a whole .atlas file from tools/atlas_astc.py, into
// the slot the model's parts name it by: every mip level is already there as
// ASTC blocks, so it goes up level by level as it is, with nothing decoded and
// no chain built. A plain texture rather than a swapchain, since nothing
// composites it: the room samples it as it draws. Also from the frame loop,
// which is where the GL context is current.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadRoomAtlas(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jobject buffer,
                                                                   jint cell, jint slot) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || buffer == NULL) {
        return;
    }
    if (slot < 0 || slot >= ROOM_MESH_ATLASES_MAX) {
        LOGW("room atlas for cell %d is in slot %d, which there is not", cell, slot);
        return;
    }
    int style = bakedStyleForCell(cell, "atlas");
    if (style == 0) {
        return;
    }
    // Whatever was in the slot is another room's or an older copy of this
    // one's, so an atlas refused below leaves the room on the void, never on
    // the wrong atlas
    releaseRoomTexture(ctx, slot);
    if (!ctx->astcSupported) {
        LOGW("room atlas for style %d is ASTC and this GPU has no "
             "GL_KHR_texture_compression_astc_ldr", style);
        return;
    }
    const unsigned char* data = (const unsigned char*)(*env)->GetDirectBufferAddress(env, buffer);
    jlong size = (*env)->GetDirectBufferCapacity(env, buffer);
    AtlasInfo info;
    if (data == NULL || size <= 0 || !atlasParse(data, (size_t)size, &info)) {
        LOGW("room atlas for style %d is not an atlas the renderer reads (%lld bytes)",
             style, (long long)size);
        return;
    }

    long started = nowNs();
    GLenum format = roomAtlasFormat(info.blockWidth);
    glGenTextures(1, &ctx->roomTextures[slot]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->roomTextures[slot]);
    // Whatever an earlier call left behind, so the check below is this atlas's
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; i++) {
    }
    // Rows run from the top of the picture, the way the model's texture
    // coordinates do, so nothing is flipped on the way in
    for (int i = 0; i < info.levels; i++) {
        glCompressedTexImage2D(GL_TEXTURE_2D, i, format,
                               (GLsizei)atlasLevelSize(info.width, i),
                               (GLsizei)atlasLevelSize(info.height, i), 0,
                               (GLsizei)info.lengths[i], data + info.offsets[i]);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, info.levels - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (ctx->roomAnisotropy > 1.0f) {
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, ctx->roomAnisotropy);
    }
    GLenum error = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    if (error != GL_NO_ERROR) {
        releaseRoomTexture(ctx, slot);
        LOGW("room atlas %ux%u for style %d, slot %d refused, GL error 0x%x",
             info.width, info.height, style, slot, error);
        return;
    }

    ctx->roomTextureStyle[slot] = style;
    ctx->roomTextureReady[slot] = 1;
    LOGEV("room atlas %ux%u ASTC %ux%u ready, style %d, slot %d, %d levels, %.1f MB, "
          "anisotropy %.0f, upload calls %.1f ms",
          info.width, info.height, info.blockWidth, info.blockHeight, style, slot, info.levels,
          (double)size / (1024.0 * 1024.0), ctx->roomAnisotropy, (nowNs() - started) / 1e6);
}
