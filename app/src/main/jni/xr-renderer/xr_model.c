// The bundled controller model: one generic controller, baked like the rooms
// and drawn into the world pass at each hand's grip, lit by a fixed light.
// World content, so it sits behind the picture's layers the way a room does.
#include "xr_renderer.h"
#include "xr_shaders.h"

// The program, the first time a model is drawn, so a session that never
// shows one never compiles it. One failure is enough to stop asking.
static int initModelProgram(XrCtx* ctx) {
    if (ctx->modelProgram != 0) {
        return 1;
    }
    if (ctx->modelProgramFailed) {
        return 0;
    }
    ctx->modelProgramFailed = 1;
    GLuint vs = compileShader(GL_VERTEX_SHADER, MODEL_VERTEX_SRC);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, MODEL_FRAGMENT_SRC);
    if (vs == 0 || fs == 0) {
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_normal");
    glBindAttribLocation(program, 2, "a_color");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        LOGE("controller model program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    ctx->modelProgram = program;
    ctx->modelViewProjUniform = glGetUniformLocation(program, "u_viewproj");
    ctx->modelMatrixUniform = glGetUniformLocation(program, "u_model");
    ctx->modelProgramFailed = 0;
    LOGI("controller model program ready");
    return 1;
}

// Where each hand's grip is this frame, and whether its model shows, read once
// a frame at the time the frame is shown, the time the eyes are drawn from.
// The input pass syncs the actions; where it returned before doing so, they
// are synced here.
void updateControllerModels(XrCtx* ctx) {
    int shown[HAND_COUNT] = { 0, 0 };
    int live = ctx->modelOn && ctx->modelReady && !ctx->modelProgramFailed && ctx->inputReady
            && ctx->gripAction != XR_NULL_HANDLE && !ctx->passthrough && ctx->shouldRender
            && ctx->sessionState == XR_SESSION_STATE_FOCUSED
            && ctx->splash.phase == SPLASH_GONE;
    if (live && !ctx->actionsSynced) {
        XrActiveActionSet active;
        active.actionSet = ctx->actionSet;
        active.subactionPath = XR_NULL_PATH;
        XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        live = XR_SUCCEEDED(xrSyncActions(ctx->session, &sync));
    }
    ctx->actionsSynced = 0;

    for (int h = 0; live && h < HAND_COUNT; h++) {
        if (ctx->gripSpaces[h] == XR_NULL_HANDLE) {
            continue;
        }
        XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
        get.action = ctx->gripAction;
        get.subactionPath = ctx->handPaths[h];
        XrActionStatePose state = { XR_TYPE_ACTION_STATE_POSE };
        int active = XR_SUCCEEDED(xrGetActionStatePose(ctx->session, &get, &state))
                && state.isActive;
        // In the space the world pass is drawn in, whatever the picture is
        // locked to: a controller is in the room, not on the head
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        if (!active || XR_FAILED(xrLocateSpace(ctx->gripSpaces[h], ctx->localSpace,
                                               ctx->predictedDisplayTime, &loc))) {
            continue;
        }
        shown[h] = controllerModelShown(ctx->modelOn, ctx->passthrough, ctx->profileKind[h],
                                        active, (unsigned)loc.locationFlags);
        if (shown[h]) {
            ctx->modelGrip[h] = loc.pose;
        }
    }

    for (int h = 0; h < HAND_COUNT; h++) {
        ctx->modelShown[h] = shown[h];
    }
    ctx->modelsShowing = shown[HAND_LEFT] || shown[HAND_RIGHT];
    int said = shown[HAND_LEFT] | (shown[HAND_RIGHT] << 1);
    if (said != ctx->modelsSaid) {
        ctx->modelsSaid = said;
        LOGI("controller models: left %s, right %s", shown[HAND_LEFT] ? "drawn" : "not drawn",
             shown[HAND_RIGHT] ? "drawn" : "not drawn");
    }
}

// Both eyes, each model at its grip, into the world pass's image after the
// room, with the room's depth so a seat or a table in front of a controller
// still hides it. Called with that pass's framebuffer bound and its depth test
// on, and leaves the buffers bound for the pass to put back.
void drawControllerModels(XrCtx* ctx) {
    ctx->modelsDrawn = 0;
    if (!ctx->modelsShowing || !initModelProgram(ctx)) {
        return;
    }
    float models[HAND_COUNT][16];
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->modelShown[h]) {
            controllerModelMatrix(ctx->modelGrip[h], h == HAND_LEFT, models[h]);
        }
    }

    glUseProgram(ctx->modelProgram);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->modelVertexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->modelIndexBuffer);
    // The vertices as the file has them: position, normal, an unused texture
    // coordinate and the colour
    GLsizei stride = ROOM_MESH_VERTEX_FLOATS * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(8 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glDisableVertexAttribArray(3);

    for (int eye = 0; eye < ROOM_EYES; eye++) {
        glViewport(eye * ctx->roomEyeWidth, 0, ctx->roomEyeWidth, ctx->roomEyeHeight);
        float proj[16];
        float view[16];
        float viewProj[16];
        projectionFromFov(proj, ctx->roomViews[eye].fov, ROOM_NEAR_M, ctx->roomFarZ);
        viewFromPose(view, ctx->roomViews[eye].pose);
        matMul(viewProj, proj, view);
        glUniformMatrix4fv(ctx->modelViewProjUniform, 1, GL_FALSE, viewProj);
        for (int h = 0; h < HAND_COUNT; h++) {
            if (!ctx->modelShown[h]) {
                continue;
            }
            glUniformMatrix4fv(ctx->modelMatrixUniform, 1, GL_FALSE, models[h]);
            glDrawElements(GL_TRIANGLES, (GLsizei)ctx->modelIndexCount, GL_UNSIGNED_INT,
                           (const void*)0);
        }
    }
    ctx->modelsDrawn = ctx->modelShown[HAND_LEFT] + ctx->modelShown[HAND_RIGHT];
}

// The model, a .room file painted from its vertex colours, read off the assets
// in Java and handed over on the frame loop, where the GL context is. Into
// buffers of its own at once, since it is small and never changes.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadControllerModel(JNIEnv* env, jobject thiz,
                                                                         jlong handle,
                                                                         jobject buffer,
                                                                         jint length) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || buffer == NULL || length <= 0 || ctx->modelReady) {
        return;
    }
    const unsigned char* data = (const unsigned char*)(*env)->GetDirectBufferAddress(env, buffer);
    if (data == NULL || (*env)->GetDirectBufferCapacity(env, buffer) < (jlong)length) {
        return;
    }
    RoomMeshInfo info;
    if (!roomMeshParse(data, (size_t)length, &info)) {
        LOGW("controller model is not a mesh the renderer reads (%d bytes)", length);
        return;
    }
    // Drawn in one go from its colours, so a part asking for an atlas would
    // come out wrong rather than missing
    if (info.atlasCount != 0 || roomMeshPaintedParts(&info) != (int)info.partCount) {
        LOGW("controller model asks for %u atlases, it is drawn from its colours alone",
             info.atlasCount);
        return;
    }
    glGenBuffers(1, &ctx->modelVertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->modelVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)info.vertexCount * ROOM_MESH_VERTEX_FLOATS * sizeof(float),
                 data + info.vertexOffset, GL_STATIC_DRAW);
    glGenBuffers(1, &ctx->modelIndexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->modelIndexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)info.indexCount * sizeof(uint32_t),
                 data + info.indexOffset, GL_STATIC_DRAW);
    // The other passes draw from client arrays with nothing bound
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    ctx->modelIndexCount = (int)info.indexCount;
    ctx->modelReady = 1;
    LOGEV("controller model ready: %u vertices, %u triangles, %d bytes, %s", info.vertexCount,
          info.indexCount / 3, length, ctx->modelOn ? "shown" : "off for now");
}

// Whether the model is drawn, the setting each session starts from, which the
// Display tab's row can change. Handed down before the first frame.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetControllerModel(JNIEnv* env, jobject thiz,
                                                                      jlong handle, jboolean on) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->modelOn = on ? 1 : 0;
    LOGEV("controller model %s at the start of the session", on ? "on" : "off");
}
