// The 3d rooms: baked models, drawn per eye into the one projection layer
// this renderer has, with the picture hung on the far wall.
#include "xr_renderer.h"
#include "xr_shaders.h"
#include "xr_atlas.h"

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
    // How wide it is hung, and the tallest it may be, 0 for no limit past the
    // room's own walls. The room sizes its own picture rather than taking the
    // size slider's, since the wall it goes on is a known size, and a picture
    // taller than 16:9 fits inside the two rather than running off the wall.
    float screenWidth;
    float screenHeight;
    // Distance at which the screen's light is down to half
    float spillRadius;
    // How much of that light a fully lit vertex takes
    float spillGain;
    // How much of the room's colour comes off its atlas, and how far down that
    // atlas is turned on the way in
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

// The home theater, a small room modelled in metres around marker nodes: a
// seated eye at (0, 1.15, 1.12) and a screen anchor centred on (0, 1.55,
// -3.132) whose scale, 3.6 by 2.025, is the largest picture the room was lit
// and seated for. The anchor is that eye raised 0.35 and brought 0.10 toward
// the screen, set by eye in a headset, and every number below is measured off
// the model and put through the same (model - anchor) * scale as the
// geometry.
static RoomParams homeTheaterParams(float scale) {
    RoomParams p;
    memset(&p, 0, sizeof(p));
    Vec3 anchor = { 0.0f, 1.15f + 0.35f, 1.12f - 0.10f };
    p.anchor = anchor;
    // The side walls at model x plus or minus 2.8, one floor throughout at
    // model y -0.14, so the picture stands on the floor the viewer does, and
    // the ceiling at 2.84
    p.halfWidth = 2.8f * scale;
    p.floorY = (-0.14f - anchor.y) * scale;
    p.screenFloorY = p.floorY;
    p.ceilingY = (2.84f - anchor.y) * scale;
    // The picture sits flat on the front wall at the anchor's z, and the back
    // wall is at model z 4.41
    p.screenZ = (-3.132f - anchor.z) * scale;
    p.backZ = (4.41f - anchor.z) * scale;
    p.screenMountY = (1.55f - anchor.y) * scale;
    p.screenProud = 0.0f;
    // Four fifths of the anchor, 2.88 m across, which sits better from the seat
    // than a picture filling the whole of it
    p.screenWidth = 3.6f * 0.8f * scale;
    p.screenHeight = 2.025f * 0.8f * scale;
    Vec3 screenAt = { 0.0f, p.screenMountY, p.screenZ };
    p.screenAt = screenAt;
    // A room a few metres across, so the light is down to half about the depth
    // of the seating. Over a painted atlas less gain than this never reads as
    // light at all.
    p.spillRadius = 3.0f * scale;
    p.spillGain = 0.55f;
    p.texMix = 1.0f;
    // Picked by eye in a headset: a little over a third of the atlas as it was
    // baked
    p.dim = 0.37f;
    p.seed = 0xc2b2ae35u;
    return p;
}

// Whether a style is one of the rooms that ship as a model
static int bakedRoomStyle(int style) {
    return style == ROOM_STYLE_THEATER;
}

// Which room a style asks for, at the scale that style is drawn
static RoomParams roomParams(int style, float scale) {
    (void)style;
    return homeTheaterParams(scale);
}

// How large a style is drawn, which is the size it was built at unless a
// property set inside the range says otherwise
static float roomScale(XrCtx* ctx, int style) {
    if (!bakedRoomStyle(style)) {
        return 1.0f;
    }
    float scale = ctx->roomScaleOverride > 0.0f ? ctx->roomScaleOverride : ROOM_THEATER_SCALE;
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
        ctx->roomPlacedStyle = 0;
        return;
    }

    // The same scale the geometry was built at, so the picture and the walls
    // around it never disagree
    RoomParams p = roomParams(style, roomScale(ctx, style));
    // The room says how big its picture is, not the size slider: the wall is a
    // known size and the picture is hung to suit it. The clamps below only
    // catch a room whose width does not fit its own wall.
    float width = p.screenWidth;
    // A room that says how tall its picture may be fits a taller one inside
    // that, keeping its shape, rather than letting it run up the wall
    if (p.screenHeight > 0.0f && aspect > 0.0f && width * aspect > p.screenHeight) {
        width = p.screenHeight / aspect;
    }
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
    // Once per room and picture shape rather than every frame
    if (style != ctx->roomPlacedStyle || fabsf(width - ctx->roomPlacedWidth) > 0.001f) {
        ctx->roomPlacedStyle = style;
        ctx->roomPlacedWidth = width;
        LOGEV("room %d hangs the picture %.2f by %.2f m, centre y %.2f z %.2f",
              style, width, height, mount, ctx->screenPose.position.z);
    }
}

// Whether the assets a baked room is made of have both arrived, and both are
// that room's. Only one room is resident, so a style whose turn it is waits
// here while its own pair is read.
static int roomAssetsReady(XrCtx* ctx, int style) {
    return ctx->roomModelReady && ctx->roomModelStyle == style
            && ctx->roomTextureReady && ctx->roomTextureStyle == style;
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
    static const float white[3] = { 1.0f, 1.0f, 1.0f };
    for (int i = 0; i < ctx->roomModelVertexCount; i++) {
        const float* src = ctx->roomModelVerts + (size_t)i * ROOM_MODEL_FLOATS;
        Vec3 pos = { (src[0] - p->anchor.x) * scale,
                     (src[1] - p->anchor.y) * scale,
                     (src[2] - p->anchor.z) * scale };
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
    // No room, which is what a room whose assets have not arrived asks for. The
    // buffers empty and the layer clears black until they do.
    if (style <= 0) {
        ctx->roomVertexCount = 0;
        ctx->roomIndexCount = 0;
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
    int maxVerts = ctx->roomModelVertexCount;
    int maxIndices = ctx->roomModelIndexCount;
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
    int ok = buildModelRoomGeometry(ctx, &params, scale, verts, indices,
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
        // A textured room has no wall shade to take this from, and its shell is
        // closed, so all this covers is the frame before the first draw
        ctx->roomClear[0] = 0.010f;
        ctx->roomClear[1] = 0.010f;
        ctx->roomClear[2] = 0.012f;
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
    // The atlas a baked room is painted with, or the white stand in, which the
    // mix below leaves out of the picture anyway. Only ever the atlas of the
    // room in the buffers, so no frame can paint one room with another's.
    int atlasOn = ctx->roomTextureReady && ctx->roomTextureStyle == ctx->roomBuiltStyle;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, atlasOn ? ctx->roomTexture : ctx->roomWhiteTexture);
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

// Whatever atlas is up, dropped. Every upload makes a fresh texture, so
// nothing of the one before, a compressed chain or its level count, can carry
// over onto the next, and a room that is going gives its memory back before
// the room arriving asks for its own.
static void releaseRoomTexture(XrCtx* ctx) {
    if (ctx->roomTexture != 0) {
        glDeleteTextures(1, &ctx->roomTexture);
        ctx->roomTexture = 0;
    }
    ctx->roomTextureReady = 0;
    ctx->roomTextureStyle = 0;
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
// has already flattened it to positions, normals and texture coordinates.
// Handed over from the frame loop, which is the thread that builds the
// geometry out of it.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadRoomModel(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jobject buffer,
                                                                   jint length, jint cell) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || buffer == NULL || length < 12) {
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
    // The room that was resident is going, so its atlas goes with it rather
    // than waiting in GL memory to be painted on this one
    if (ctx->roomModelStyle != style) {
        releaseRoomTexture(ctx);
    }
    // Kept in the model's own space. The anchor and the scale go on as the
    // geometry is built, so the scale can move without this being read again.
    free(ctx->roomModelVerts);
    free(ctx->roomModelIndices);
    ctx->roomModelVerts = verts;
    ctx->roomModelIndices = indices;
    ctx->roomModelVertexCount = (int)vertexCount;
    ctx->roomModelIndexCount = (int)indexCount;
    ctx->roomModelStyle = style;
    ctx->roomModelReady = 1;
    LOGEV("room model ready, style %d, %u vertices, %u indices", style, vertexCount, indexCount);
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

// A room's atlas, a whole .atlas file from tools/atlas_astc.py: every mip level
// is already there as ASTC blocks, so it goes up level by level as it is, with
// nothing decoded and no chain built. A plain texture rather than a swapchain,
// since nothing composites it: the room samples it as it draws. Also from the
// frame loop, which is where the GL context is current.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadRoomAtlas(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jobject buffer,
                                                                   jint cell) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || buffer == NULL) {
        return;
    }
    int style = bakedStyleForCell(cell, "atlas");
    if (style == 0) {
        return;
    }
    // Whatever was up is another room's or an older copy of this one's, so an
    // atlas refused below leaves the room on its stand in, never on the wrong
    // atlas
    releaseRoomTexture(ctx);
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
    glGenTextures(1, &ctx->roomTexture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->roomTexture);
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
        releaseRoomTexture(ctx);
        LOGW("room atlas %ux%u for style %d refused, GL error 0x%x",
             info.width, info.height, style, error);
        return;
    }

    ctx->roomTextureStyle = style;
    ctx->roomTextureReady = 1;
    LOGEV("room atlas %ux%u ASTC %ux%u ready, style %d, %d levels, %.1f MB, "
          "anisotropy %.0f, upload calls %.1f ms",
          info.width, info.height, info.blockWidth, info.blockHeight, style, info.levels,
          (double)size / (1024.0 * 1024.0), ctx->roomAnisotropy, (nowNs() - started) / 1e6);
}
