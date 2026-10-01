// Controllers, hands and gaze: the actions they are read through, the ray
// they aim, and the per frame pass that turns it into hover, grab, panel
// presses and host mouse events.
#include "xr_renderer.h"

// A pinch is how these headsets click, but it is not always offered as an
// input to bind to. The joints always are, so it is measured here instead:
// thumb tip to index tip, gated in xr_pinch.c so it has to be meant.

static void initJointTracking(XrCtx* ctx) {
    if (!ctx->handTracking) {
        return;
    }
    if (XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrCreateHandTrackerEXT",
                                        (PFN_xrVoidFunction*)&ctx->pfnCreateHandTracker))
            || XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrDestroyHandTrackerEXT",
                                               (PFN_xrVoidFunction*)&ctx->pfnDestroyHandTracker))
            || XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrLocateHandJointsEXT",
                                               (PFN_xrVoidFunction*)&ctx->pfnLocateHandJoints))
            || ctx->pfnCreateHandTracker == NULL || ctx->pfnLocateHandJoints == NULL) {
        LOGW("hand joint entry points missing");
        ctx->jointTracking = 0;
        return;
    }

    for (int h = 0; h < HAND_COUNT; h++) {
        XrHandTrackerCreateInfoEXT info = { XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT };
        info.hand = h == HAND_LEFT ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        if (!checkXr(ctx->pfnCreateHandTracker(ctx->session, &info, &ctx->handTrackers[h]),
                     "create hand tracker")) {
            ctx->handTrackers[h] = XR_NULL_HANDLE;
            return;
        }
    }
    ctx->jointTracking = 1;
    LOGI("reading hand joints for pinch");
}

// Which kind of thing is driving each hand. Hands are never still enough for
// the movement gate to mean anything, so they wake the pointer a different way
// and need to be told apart from controllers.
void refreshInputSource(XrCtx* ctx) {
    if (ctx->session == XR_NULL_HANDLE || !ctx->inputReady) {
        return;
    }
    for (int h = 0; h < HAND_COUNT; h++) {
        XrInteractionProfileState state = { XR_TYPE_INTERACTION_PROFILE_STATE };
        if (XR_FAILED(xrGetCurrentInteractionProfile(ctx->session, ctx->handPaths[h], &state))) {
            continue;
        }
        XrPath profile = state.interactionProfile;
        int kind = profileKind(profile != XR_NULL_PATH,
                               profile == ctx->handProfile || profile == ctx->msftHandProfile);
        // Without a pinch bound there is nothing to wake the pointer with, so
        // those hands stay on the movement gate rather than becoming unusable
        ctx->usingHands[h] = ctx->handClickOk && kind == PROFILE_HANDS;
        ctx->onExtHands[h] = kind == PROFILE_HANDS && profile == ctx->handProfile;
        if (kind != ctx->profileKind[h]) {
            ctx->profileKind[h] = kind;
            // The rest clock belongs to whatever was on that hand, so a
            // controller's last movement must not go on counting for a hand
            controllerClockReset(&ctx->aimClock[h]);
            LOGI("hand %d profile: %s", h, kind == PROFILE_CONTROLLER ? "controller"
                                           : (kind == PROFILE_HANDS ? "hands" : "none"));
        }
    }
}

static XrPath toPath(XrCtx* ctx, const char* str) {
    XrPath path = XR_NULL_PATH;
    xrStringToPath(ctx->instance, str, &path);
    return path;
}

static XrAction makeAction(XrCtx* ctx, XrActionType type, const char* name, const char* label) {
    XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
    info.actionType = type;
    strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(info.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.countSubactionPaths = HAND_COUNT;
    info.subactionPaths = ctx->handPaths;

    XrAction action = XR_NULL_HANDLE;
    if (!checkXr(xrCreateAction(ctx->actionSet, &info, &action), name)) {
        return XR_NULL_HANDLE;
    }
    return action;
}

// One unsupported path rejects a whole profile, so the full set is offered
// first and a runtime that does not recognise this controller falls back to
// aim and trigger, which every profile has.
static void suggestBindings(XrCtx* ctx, const char* profile, int full) {
    XrActionSuggestedBinding b[16];
    uint32_t n = 0;
    static const char* hands[HAND_COUNT] = { "/user/hand/left", "/user/hand/right" };
    // x and y on the left controller, a and b on the right
    static const char* rightClick[HAND_COUNT] = { "input/x/click", "input/a/click" };
    static const char* middleClick[HAND_COUNT] = { "input/y/click", "input/b/click" };
    int simple = strstr(profile, "/khr/") != NULL;

    for (int h = 0; h < HAND_COUNT; h++) {
        char path[XR_MAX_PATH_LENGTH];

        snprintf(path, sizeof(path), "%s/input/aim/pose", hands[h]);
        b[n].action = ctx->aimAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/%s", hands[h],
                 simple ? "input/select/click" : "input/trigger/value");
        b[n].action = ctx->triggerAction;
        b[n++].binding = toPath(ctx, path);

        if (!full || simple) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", hands[h], rightClick[h]);
        b[n].action = ctx->rightClickAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/%s", hands[h], middleClick[h]);
        b[n].action = ctx->middleClickAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/input/thumbstick", hands[h]);
        b[n].action = ctx->scrollAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/input/thumbstick/click", hands[h]);
        b[n].action = ctx->toggleAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/input/squeeze/value", hands[h]);
        b[n].action = ctx->grabAction;
        b[n++].binding = toPath(ctx, path);
    }

    XrInteractionProfileSuggestedBinding suggest = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggest.interactionProfile = toPath(ctx, profile);
    suggest.countSuggestedBindings = n;
    suggest.suggestedBindings = b;

    XrResult res = xrSuggestInteractionProfileBindings(ctx->instance, &suggest);
    if (XR_SUCCEEDED(res)) {
        LOGI("bindings accepted for %s (%s)", profile, full ? "full" : "reduced");
    }
    else if (full) {
        LOGW("full bindings rejected for %s (%d), trying aim and trigger only", profile, res);
        suggestBindings(ctx, profile, 0);
    }
    else {
        LOGW("bindings rejected for %s (%d)", profile, res);
    }
}

// Hands come in through the same actions the controllers use, so everything
// downstream of here treats them identically: same ray, same handles, same
// picker. Only the paths differ, which is why this is its own function rather
// than another flag on the one above.
static XrResult trySuggestHands(XrCtx* ctx, const char* profile, const char* aim,
                                const char* click, const char* grasp) {
    XrActionSuggestedBinding b[6];
    uint32_t n = 0;
    static const char* hands[HAND_COUNT] = { "/user/hand/left", "/user/hand/right" };

    for (int h = 0; h < HAND_COUNT; h++) {
        char path[XR_MAX_PATH_LENGTH];

        snprintf(path, sizeof(path), "%s/%s", hands[h], aim);
        b[n].action = ctx->aimAction;
        b[n++].binding = toPath(ctx, path);

        if (click != NULL) {
            snprintf(path, sizeof(path), "%s/%s", hands[h], click);
            b[n].action = ctx->triggerAction;
            b[n++].binding = toPath(ctx, path);
        }

        if (grasp != NULL) {
            snprintf(path, sizeof(path), "%s/%s", hands[h], grasp);
            b[n].action = ctx->grabAction;
            b[n++].binding = toPath(ctx, path);
        }
    }

    XrInteractionProfileSuggestedBinding suggest = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggest.interactionProfile = toPath(ctx, profile);
    suggest.countSuggestedBindings = n;
    suggest.suggestedBindings = b;

    return xrSuggestInteractionProfileBindings(ctx->instance, &suggest);
}

// Runtimes that offer the hand profile do not all implement every input in it,
// and one unsupported path throws out the whole suggestion. So the inputs are
// offered up in falling order of usefulness until a set is accepted. Returns
// whether a pinch ended up bound, since without one the hands cannot wake the
// pointer and are better left to the movement gate.
static int suggestHandBindings(XrCtx* ctx, const char* profile, const char* aim,
                               const char* const* clicks, int clickCount,
                               const char* grasp) {
    XrResult res = XR_SUCCESS;
    for (int c = 0; c < clickCount; c++) {
        if (grasp != NULL) {
            res = trySuggestHands(ctx, profile, aim, clicks[c], grasp);
            if (XR_SUCCEEDED(res)) {
                LOGI("hand bindings accepted for %s (%s and grasp)", profile, clicks[c]);
                return 1;
            }
        }
        res = trySuggestHands(ctx, profile, aim, clicks[c], NULL);
        if (XR_SUCCEEDED(res)) {
            LOGI("hand bindings accepted for %s (%s)", profile, clicks[c]);
            return 1;
        }
    }
    res = trySuggestHands(ctx, profile, aim, NULL, NULL);
    if (XR_SUCCEEDED(res)) {
        LOGW("only the aim pose bound for %s, so hands cannot click", profile);
        return 0;
    }
    LOGW("hand bindings rejected for %s, even the aim pose alone (%d)", profile, res);
    return 0;
}

int initXrInput(XrCtx* ctx) {
    XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy(setInfo.actionSetName, "moonlight", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(setInfo.localizedActionSetName, "Moonlight", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (!checkXr(xrCreateActionSet(ctx->instance, &setInfo, &ctx->actionSet), "create action set")) {
        return 0;
    }

    ctx->handPaths[HAND_LEFT] = toPath(ctx, "/user/hand/left");
    ctx->handPaths[HAND_RIGHT] = toPath(ctx, "/user/hand/right");

    ctx->aimAction = makeAction(ctx, XR_ACTION_TYPE_POSE_INPUT, "aim", "Pointer");
    ctx->triggerAction = makeAction(ctx, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Left click");
    ctx->rightClickAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "rightclick", "Right click");
    ctx->middleClickAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "middleclick", "Middle click");
    ctx->scrollAction = makeAction(ctx, XR_ACTION_TYPE_VECTOR2F_INPUT, "scroll", "Scroll");
    ctx->grabAction = makeAction(ctx, XR_ACTION_TYPE_FLOAT_INPUT, "grab", "Move the screen");
    ctx->toggleAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "pointertoggle", "Pointer on or off");

    if (ctx->aimAction == XR_NULL_HANDLE || ctx->triggerAction == XR_NULL_HANDLE) {
        return 0;
    }

    suggestBindings(ctx, "/interaction_profiles/khr/simple_controller", 1);
    suggestBindings(ctx, "/interaction_profiles/oculus/touch_controller", 1);
    if (ctx->picoInteraction) {
        suggestBindings(ctx, "/interaction_profiles/bytedance/pico4_controller", 1);
    }

    // Hands. aim_activate is the spec's own name for pointing at something out
    // of reach and pinching to act on it, which is exactly what the ray does.
    // Gaze is its own top level path rather than a hand, so it needs an action
    // of its own. There is no click on it: whatever the runtime reports as a
    // trigger, usually a pinch, does the clicking.
    if (ctx->eyeGaze) {
        XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
        info.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strncpy(info.actionName, "gaze", XR_MAX_ACTION_NAME_SIZE - 1);
        strncpy(info.localizedActionName, "Gaze pointer",
                XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        if (checkXr(xrCreateAction(ctx->actionSet, &info, &ctx->gazeAction), "gaze action")) {
            XrActionSuggestedBinding b;
            b.action = ctx->gazeAction;
            b.binding = toPath(ctx, "/user/eyes_ext/input/gaze_ext/pose");

            XrInteractionProfileSuggestedBinding suggest = {
                XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING
            };
            suggest.interactionProfile = toPath(ctx,
                    "/interaction_profiles/ext/eye_gaze_interaction");
            suggest.countSuggestedBindings = 1;
            suggest.suggestedBindings = &b;
            if (XR_FAILED(xrSuggestInteractionProfileBindings(ctx->instance, &suggest))) {
                LOGW("gaze bindings rejected");
                ctx->gazeAction = XR_NULL_HANDLE;
                ctx->eyeGaze = 0;
            }
        }
        else {
            ctx->gazeAction = XR_NULL_HANDLE;
            ctx->eyeGaze = 0;
        }
    }

    if (ctx->handInteraction) {
        // aim_activate is the spec's own name for the far pointer pinch, and
        // pinch is the plain one. Runtimes vary in which they implement.
        static const char* const clicks[] = {
            "input/aim_activate_ext/value", "input/pinch_ext/value"
        };
        const char* profile = "/interaction_profiles/ext/hand_interaction_ext";
        ctx->extHandClick = suggestHandBindings(ctx, profile, "input/aim_ext/pose",
                                                clicks, 2, "input/grasp_ext/value");
        ctx->handClickOk |= ctx->extHandClick;
        ctx->handProfile = toPath(ctx, profile);
    }
    // Older runtimes that predate the EXT profile. Same idea, fewer inputs.
    if (ctx->msftHandInteraction) {
        static const char* const clicks[] = { "input/select/value" };
        const char* profile = "/interaction_profiles/microsoft/hand_interaction";
        ctx->handClickOk |= suggestHandBindings(ctx, profile, "input/aim/pose",
                                                clicks, 1, "input/squeeze/value");
        ctx->msftHandProfile = toPath(ctx, profile);
    }

    XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &ctx->actionSet;
    if (!checkXr(xrAttachSessionActionSets(ctx->session, &attach), "attach action sets")) {
        return 0;
    }

    for (int h = 0; h < HAND_COUNT; h++) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = ctx->aimAction;
        spaceInfo.subactionPath = ctx->handPaths[h];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!checkXr(xrCreateActionSpace(ctx->session, &spaceInfo, &ctx->aimSpaces[h]),
                     "create aim space")) {
            return 0;
        }
    }

    if (ctx->gazeAction != XR_NULL_HANDLE) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = ctx->gazeAction;
        spaceInfo.subactionPath = XR_NULL_PATH;
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!checkXr(xrCreateActionSpace(ctx->session, &spaceInfo, &ctx->aimSpaces[SRC_GAZE]),
                     "create gaze space")) {
            ctx->aimSpaces[SRC_GAZE] = XR_NULL_HANDLE;
        }
    }

    ctx->inputReady = 1;
    ctx->pointerOn = 1;
    initJointTracking(ctx);
    refreshInputSource(ctx);
    LOGI("controller input ready (pico bindings %s, hand pinch %s)",
         ctx->picoInteraction ? "offered" : "not offered by this runtime",
         ctx->handClickOk ? "bound" : (ctx->jointTracking ? "from joints" : "unavailable"));
    LOGEV("pinch: joints on %.0f mm, off %.0f mm, closing %.0f mm in %ld ms (untracked tips "
          "%.0f / %.0f mm), hold %ld ms; runtime value on %.1f off %.1f, %s",
          PINCH_ON_M * 1000.0f, PINCH_OFF_M * 1000.0f, PINCH_CLOSE_M * 1000.0f,
          PINCH_CLOSE_WINDOW_NS / 1000000L, PINCH_LOOSE_ON_M * 1000.0f,
          PINCH_LOOSE_OFF_M * 1000.0f, PINCH_HOLD_NS / 1000000L, PINCH_VALUE_ON,
          PINCH_VALUE_OFF, ctx->extHandClick ? "the EXT profile's alone where it is on a hand"
                                             : "or the joints, with the hold");
    return 1;
}

static float actionFloat(XrCtx* ctx, XrAction action, int hand) {
    if (action == XR_NULL_HANDLE) {
        return 0.0f;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
    if (XR_FAILED(xrGetActionStateFloat(ctx->session, &get, &state)) || !state.isActive) {
        return 0.0f;
    }
    return state.currentState;
}

static int actionBool(XrCtx* ctx, XrAction action, int hand) {
    if (action == XR_NULL_HANDLE) {
        return 0;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
    if (XR_FAILED(xrGetActionStateBoolean(ctx->session, &get, &state)) || !state.isActive) {
        return 0;
    }
    return state.currentState != 0;
}

static XrVector2f actionVec2(XrCtx* ctx, XrAction action, int hand) {
    XrVector2f zero = { 0.0f, 0.0f };
    if (action == XR_NULL_HANDLE) {
        return zero;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_FAILED(xrGetActionStateVector2f(ctx->session, &get, &state)) || !state.isActive) {
        return zero;
    }
    return state.currentState;
}

// Whether the runtime is actually driving a pose action on this hand. A
// controller that is off, asleep or gone leaves it inactive.
static int actionPoseActive(XrCtx* ctx, XrAction action, int hand) {
    if (action == XR_NULL_HANDLE) {
        return 0;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = ctx->handPaths[hand];

    XrActionStatePose state = { XR_TYPE_ACTION_STATE_POSE };
    return XR_SUCCEEDED(xrGetActionStatePose(ctx->session, &get, &state)) && state.isActive;
}

static int stickPushed(XrVector2f stick) {
    return fabsf(stick.x) > SCROLL_DEADZONE || fabsf(stick.y) > SCROLL_DEADZONE;
}

// A pointer ray from the joints, for runtimes that track hands but never offer
// a pointer pose. Cast from a shoulder rather than from the hand itself: a ray
// along the finger swings wildly with small movements of the wrist, while one
// through the hand from the shoulder is what the arm is actually aiming and is
// steady enough to hold on a target.
static void buildHandRay(XrCtx* ctx, int hand, const XrPosef* head,
                         const XrHandJointLocationEXT* joints) {
    const XrHandJointLocationEXT* knuckle = &joints[XR_HAND_JOINT_INDEX_PROXIMAL_EXT];
    if (!(knuckle->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        ctx->handRayValid[hand] = 0;
        return;
    }

    Vec3 offset = { hand == HAND_RIGHT ? 0.17f : -0.17f, -0.20f, 0.05f };
    Vec3 shoulder = quatRotate(head->orientation, offset);
    shoulder.x += head->position.x;
    shoulder.y += head->position.y;
    shoulder.z += head->position.z;

    Vec3 origin = { knuckle->pose.position.x, knuckle->pose.position.y,
                    knuckle->pose.position.z };
    Vec3 dir = vecSub(origin, shoulder);
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 0.05f) {
        ctx->handRayValid[hand] = 0;
        return;
    }
    dir = vecNorm(dir);

    // A pose points down its own -Z, so the basis is built around that
    Vec3 worldUp = { 0.0f, 1.0f, 0.0f };
    Vec3 rayZ = { -dir.x, -dir.y, -dir.z };
    Vec3 rayX = vecCross(worldUp, rayZ);
    float side = sqrtf(rayX.x * rayX.x + rayX.y * rayX.y + rayX.z * rayX.z);
    if (side < 0.01f) {
        Vec3 fallback = { 1.0f, 0.0f, 0.0f };
        rayX = vecCross(fallback, rayZ);
    }
    rayX = vecNorm(rayX);
    Vec3 rayY = vecCross(rayZ, rayX);

    ctx->handRay[hand].orientation = quatFromBasis(rayX, rayY, rayZ);
    ctx->handRay[hand].position = knuckle->pose.position;
    ctx->handRayValid[hand] = 1;
}

// How a pose located in the local space is moved into the frame the screen is
// in. Head locked, the screen hangs in the head's frame. Every locate is still
// made against the local space, as it is with the screen in the room, and the
// pose is moved into the head's frame here by undoing the head's own pose,
// rather than asking the runtime for poses relative to the view space: the two
// paths then make exactly the same runtime calls. The pointer went missing
// head locked on the Pico 4 Ultra (#20), and locates relative to the view
// space were the only calls that path made differently. With the head not
// located nothing can be moved into its frame.
typedef struct {
    int toHead;
    int ok;
    XrPosef head;
} FrameXform;

static int intoFrame(const FrameXform* x, XrPosef* p) {
    if (!x->toHead) {
        return 1;
    }
    if (!x->ok) {
        return 0;
    }
    *p = poseInFrame(x->head, *p);
    return 1;
}

// How far one joint is from another, or -1 where either is not where the
// runtime can say
static float tipGap(const XrHandJointLocationEXT* a, const XrHandJointLocationEXT* b) {
    if (!(a->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
            || !(b->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        return -1.0f;
    }
    float dx = a->pose.position.x - b->pose.position.x;
    float dy = a->pose.position.y - b->pose.position.y;
    float dz = a->pose.position.z - b->pose.position.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// The tips the lock gesture is judged on: the ring tip to the thumb, with the
// index and middle tips' own gaps to it, which have to stay clear
static void readRingTips(XrCtx* ctx, int hand, const XrHandJointLocationEXT* joints) {
    const XrSpaceLocationFlags tracked = XR_SPACE_LOCATION_POSITION_VALID_BIT
            | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    const XrHandJointLocationEXT* thumb = &joints[XR_HAND_JOINT_THUMB_TIP_EXT];
    const XrHandJointLocationEXT* ring = &joints[XR_HAND_JOINT_RING_TIP_EXT];
    ctx->ringGap[hand] = tipGap(thumb, ring);
    ctx->indexGap[hand] = tipGap(thumb, &joints[XR_HAND_JOINT_INDEX_TIP_EXT]);
    ctx->middleGap[hand] = tipGap(thumb, &joints[XR_HAND_JOINT_MIDDLE_TIP_EXT]);
    // A deliberate gesture wants the two tips that touch actually seen, and
    // the other two at least placed
    ctx->ringTipsTracked[hand] = (thumb->locationFlags & tracked) == tracked
            && (ring->locationFlags & tracked) == tracked
            && ctx->indexGap[hand] >= 0.0f && ctx->middleGap[hand] >= 0.0f;
}

static int jointPinching(XrCtx* ctx, int hand, const FrameXform* xform, const XrPosef* head,
                         int headValid, long nowNs) {
    ctx->ringTipsTracked[hand] = 0;
    if (!ctx->jointTracking || ctx->handTrackers[hand] == XR_NULL_HANDLE) {
        ctx->handRayValid[hand] = 0;
        return 0;
    }

    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
    XrHandJointLocationsEXT locations = { XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
    locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
    locations.jointLocations = joints;

    XrHandJointsLocateInfoEXT locate = { XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
    locate.baseSpace = ctx->localSpace;
    locate.time = ctx->predictedDisplayTime;
    float closed = 0.0f;
    int located = XR_SUCCEEDED(ctx->pfnLocateHandJoints(ctx->handTrackers[hand], &locate,
                                                         &locations))
            && locations.isActive;
    // Into the screen's frame, all of them, so the ray and the pinch point
    // come out where the aim poses are
    for (int j = 0; located && j < XR_HAND_JOINT_COUNT_EXT; j++) {
        located = intoFrame(xform, &joints[j].pose);
    }
    if (!located) {
        pinchGateStep(&ctx->pinchGate[hand], 0, 0, 0.0f, nowNs, &closed);
        ctx->pinchPointValid[hand] = 0;
        ctx->handRayValid[hand] = 0;
        return 0;
    }

    if (headValid) {
        buildHandRay(ctx, hand, head, joints);
    }
    else {
        // No shoulder to cast from, and last frame's ray is stale
        ctx->handRayValid[hand] = 0;
    }
    readRingTips(ctx, hand, joints);

    const XrSpaceLocationFlags tracked = XR_SPACE_LOCATION_POSITION_VALID_BIT
            | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    const XrHandJointLocationEXT* thumb = &joints[XR_HAND_JOINT_THUMB_TIP_EXT];
    const XrHandJointLocationEXT* index = &joints[XR_HAND_JOINT_INDEX_TIP_EXT];
    int valid = (thumb->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
            && (index->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    if (!valid) {
        pinchGateStep(&ctx->pinchGate[hand], 0, 0, 0.0f, nowNs, &closed);
        ctx->pinchPointValid[hand] = 0;
        return 0;
    }

    float dx = thumb->pose.position.x - index->pose.position.x;
    float dy = thumb->pose.position.y - index->pose.position.y;
    float dz = thumb->pose.position.z - index->pose.position.z;
    float gap = sqrtf(dx * dx + dy * dy + dz * dz);

    // Where the pinch happened, which is what a drag follows
    ctx->pinchPoint[hand].x = (thumb->pose.position.x + index->pose.position.x) * 0.5f;
    ctx->pinchPoint[hand].y = (thumb->pose.position.y + index->pose.position.y) * 0.5f;
    ctx->pinchPoint[hand].z = (thumb->pose.position.z + index->pose.position.z) * 0.5f;
    ctx->pinchPointValid[hand] = 1;

    // Tracked as well as valid takes the strict rule. A runtime estimating
    // the tips gives nothing trustworthy to judge the closing by.
    PinchGate* gate = &ctx->pinchGate[hand];
    int both = (thumb->locationFlags & tracked) == tracked
            && (index->locationFlags & tracked) == tracked;
    int was = gate->down;
    float lastGap = gate->count > 0 ? gate->gap[(gate->next + PINCH_RING - 1) % PINCH_RING]
                                    : 1.0f;
    int down = pinchGateStep(gate, 1, both, gap, nowNs, &closed);
    // Said where the tips first come inside the on distance, so a refusal
    // shows once per approach rather than every frame a hand rests closed
    if (both && !was && gap < PINCH_ON_M && lastGap >= PINCH_ON_M) {
        LOGI("hand %d joint pinch %s, tips closed %.0f mm in the last %ld ms", hand,
             down ? "on" : "refused", closed * 1000.0f, PINCH_CLOSE_WINDOW_NS / 1000000L);
    }
    return down;
}

// The sliders place the screen, the grab moves it from there. Moving either
// slider is taken as the user asking for the placement back. Says whether it
// reseeded, since a room holding the screen has to know the placement waiting
// behind it has changed.
int updatePlacement(XrCtx* ctx, float distance, float quadWidth, float curvature) {
    int sliderMoved = ctx->sliderSeen
            && (fabsf(distance - ctx->lastDistance) > 1e-4f
                || fabsf(quadWidth - ctx->lastQuadWidth) > 1e-4f);
    int reseeded = !ctx->placementValid || sliderMoved;

    if (reseeded) {
        memset(&ctx->screenPose, 0, sizeof(ctx->screenPose));
        ctx->screenPose.orientation.w = 1.0f;
        ctx->screenPose.position.z = -distance;
        ctx->screenWidth = quadWidth;
        // Radius runs from 4x distance (slightly curved) down to the distance
        // itself (wrapped around the viewer) as curvature rises
        ctx->screenRadius = distance * (1.0f + 3.0f * (1.0f - curvature));
        ctx->placementValid = 1;
        ctx->grabMode = GRAB_NONE;
        ctx->poseDirty = 1;
    }

    ctx->lastDistance = distance;
    ctx->lastQuadWidth = quadWidth;
    ctx->sliderSeen = 1;
    return reseeded;
}

// Handed back only when a grab ends, so preferences are written once per move
// rather than every frame of it
static void writeInputPose(XrCtx* ctx, float* out) {
    if (!ctx->poseDirty) {
        return;
    }
    ctx->poseDirty = 0;
    out[IN_POSE_DIRTY] = 1.0f;
    out[IN_POSE + 0] = ctx->screenPose.position.x;
    out[IN_POSE + 1] = ctx->screenPose.position.y;
    out[IN_POSE + 2] = ctx->screenPose.position.z;
    out[IN_POSE + 3] = ctx->screenPose.orientation.x;
    out[IN_POSE + 4] = ctx->screenPose.orientation.y;
    out[IN_POSE + 5] = ctx->screenPose.orientation.z;
    out[IN_POSE + 6] = ctx->screenPose.orientation.w;
    out[IN_POSE + 7] = ctx->screenWidth;
    out[IN_POSE + 8] = ctx->screenRadius;
    out[IN_POSE + 9] = ctx->panelCurve;
}

// How far along the half diagonal to the held corner a point on the picture
// is, as it stood when the grab began: 0 at the centre, 1 on the corner.
// Measured from the centre, since that is what holds. The half diagonal is the
// held corner's own position, so projecting onto it keeps that corner under
// the ray. Measuring from the far corner along the whole diagonal, as this did
// when that corner was the anchor, would leave the bracket creeping out at half
// the speed of the hand.
static float diagonalReachAt(XrCtx* ctx, float px, float py) {
    float halfX = -ctx->grabOppX;
    float halfY = -ctx->grabOppY;
    float halfLen = halfX * halfX + halfY * halfY;
    return halfLen > 0.0f ? (px * halfX + py * halfY) / halfLen : 1.0f;
}

static float diagonalReach(XrCtx* ctx, float u, float v) {
    return diagonalReachAt(ctx, (u - 0.5f) * ctx->grabWidth, (0.5f - v) * ctx->grabHeight);
}

// Gaze clicks with a pinch, so the source that pointed says nothing about
// which hand pressed. This is that hand: the one pinching now, right first.
static int pinchingHand(XrCtx* ctx) {
    if (ctx->triggerEdge[HAND_RIGHT]) {
        return HAND_RIGHT;
    }
    if (ctx->triggerEdge[HAND_LEFT]) {
        return HAND_LEFT;
    }
    if (ctx->triggerDown[HAND_RIGHT]) {
        return HAND_RIGHT;
    }
    if (ctx->triggerDown[HAND_LEFT]) {
        return HAND_LEFT;
    }
    return -1;
}

// Whether the joints are still giving a pinch to follow. Where the runtime's
// value is the press, the pinch is whatever it says, and the joints only give
// the point.
static int pinchTracked(XrCtx* ctx, int hand) {
    if (hand < 0 || hand >= HAND_COUNT || !ctx->pinchPointValid[hand]) {
        return 0;
    }
    if (ctx->extHandClick && ctx->onExtHands[hand]) {
        return ctx->triggerDown[hand];
    }
    return ctx->pinchGate[hand].down;
}

// Where the hand carrying a drag is: the pinch itself, since that is the part
// the user feels moving, and the aim only where the joints give no pinch
static Vec3 gazeHandPos(XrCtx* ctx, const XrPosef* aims, int hand) {
    if (pinchTracked(ctx, hand)) {
        return ctx->pinchPoint[hand];
    }
    Vec3 p = { aims[hand].position.x, aims[hand].position.y, aims[hand].position.z };
    return p;
}

// The eyes chose the target, the hand that pinched moves it. Hands back that
// hand, with where it started and how much the target is geared up by, or -1
// if nothing is pinching.
static int gazeDragHand(XrCtx* ctx, const XrPosef* aims, const int* valid, Vec3 target,
                        Vec3* outStart, float* outScale) {
    int hand = pinchingHand(ctx);
    if (hand < 0 || (!valid[hand] && !pinchTracked(ctx, hand))) {
        return -1;
    }
    Vec3 handPos = gazeHandPos(ctx, aims, hand);
    Vec3 head = { ctx->headPos.x, ctx->headPos.y, ctx->headPos.z };
    *outStart = handPos;
    *outScale = dragScale(head, target, handPos);
    return hand;
}

// How far the hand carrying a drag the eyes started has taken it, eased in
// from the pinch and held while the head turns
static Vec3 gazeDragCarry(XrCtx* ctx, DragRamp* ramp, Vec3 travel, long nowNs) {
    int held = 0;
    Vec3 carry = dragRampStep(ramp, travel, ctx->headTurnRate, nowNs, &held);
    if (held && !ctx->dragHeldByHead) {
        LOGI("drag held, head turning %.0f deg/s", ctx->headTurnRate);
    }
    ctx->dragHeldByHead = held;
    return carry;
}

// Move and resize both work off the handle the ray was over when the grip
// closed. Gripping the picture itself does nothing, which keeps the panel from
// being dragged by accident while pointing at something.
static void applyGrab(XrCtx* ctx, XrPosef* aims, const int* valid, const float* grab, int hand,
                      int hover, int corner, int offPicture, float height, int curved) {
    for (int h = 0; h < HAND_COUNT; h++) {
        int wasDown = ctx->grabDown[h];
        ctx->grabDown[h] = grab[h] > (wasDown ? PRESS_OFF : PRESS_ON);
        ctx->gripEdge[h] = ctx->grabDown[h] && !wasDown;
    }

    if (ctx->grabMode != GRAB_NONE) {
        int stillHeld = ctx->grabByTrigger ? ctx->triggerDown[ctx->grabHand]
                                           : ctx->grabDown[ctx->grabHand];
        // A grab the eyes started is carried by the pinch, which the joints
        // keep reporting where the hand's own ray has dropped out
        int stillThere = valid[ctx->grabHand]
                || (ctx->grabByGaze && pinchTracked(ctx, ctx->grabHand));
        if (!stillHeld || !stillThere) {
            // Persist where it ended up, not every frame of the drag
            ctx->grabMode = GRAB_NONE;
            ctx->grabByGaze = 0;
            ctx->poseDirty = 1;
            return;
        }
    }

    if (ctx->grabMode == GRAB_NONE) {
        // A 3d room holds the picture on its wall and forces the pose every
        // frame, so a move could only fight it and the bar is not drawn there.
        // A room that lets its picture be resized has corners, and those set
        // how much of the room's screen the picture fills; in any other room
        // the corners are not even hovered.
        int roomStyle = roomEffective(ctx);
        if (roomStyle > 0 && (hover != HOVER_CORNER || !roomResizable(roomStyle))) {
            return;
        }
        if (hand < 0 || (hover != HOVER_BAR && hover != HOVER_CORNER)) {
            return;
        }

        // Apps disagree about which button grabs, so both do. The trigger only
        // counts where the handle hangs outside the picture, since inside it is
        // a left click and the bottom corners of a desktop are worth clicking.
        int byGrip = ctx->gripEdge[hand];
        int byTrigger = ctx->triggerEdge[hand] && offPicture;
        if (!byGrip && !byTrigger) {
            return;
        }
        ctx->grabByTrigger = !byGrip;

        // Eyes pick the handle up, a hand carries it. Run off the eye ray the
        // picture would follow wherever the user looked next.
        int mover = hand;
        ctx->grabByGaze = hand == SRC_GAZE;
        if (ctx->grabByGaze) {
            float gu, gv;
            if (!screenProject(aims[hand], ctx->screenPose, ctx->screenWidth, height,
                               ctx->screenRadius, curved, &gu, &gv)) {
                ctx->grabByGaze = 0;
                return;
            }
            Vec3 target = screenPoint(gu, gv, ctx->screenPose, ctx->screenWidth, height,
                                      ctx->screenRadius, curved);
            mover = gazeDragHand(ctx, aims, valid, target, &ctx->grabHandStart,
                                 &ctx->grabScale);
            if (mover < 0) {
                ctx->grabByGaze = 0;
                return;
            }
            dragRampStart(&ctx->grabRamp, ctx->lastInputNs);
            LOGI("gaze grab: %s by hand %d, scale %.2f", hover == HOVER_BAR ? "move" : "resize",
                 mover, ctx->grabScale);
        }

        ctx->grabHand = mover;
        ctx->grabAim = aims[mover];
        ctx->grabScreen = ctx->screenPose;
        ctx->grabWidth = ctx->screenWidth;
        ctx->grabHeight = height;
        ctx->grabRadius = ctx->screenRadius;

        if (hover == HOVER_BAR) {
            ctx->grabMode = GRAB_MOVE;
            // Read once here rather than every frame of the drag. grabScreen
            // is the pose these come off, and it was just set from screenPose.
            // Re-extracting each frame would let the rounding walk the tilt
            // away over a long move.
            ctx->grabPitch = screenPitch(ctx);
            ctx->grabRoll = screenRoll(ctx);
            return;
        }

        // Outside a room the hit itself is not needed any more, but a ray that
        // misses the plane has nothing to measure the drag against
        float u, v;
        if (!screenProject(aims[hand], ctx->grabScreen, ctx->screenWidth, height,
                           ctx->screenRadius, curved, &u, &v)) {
            return;
        }

        // The centre is the anchor, and the drag runs along the half diagonal
        // out to the corner being held
        int right = (corner == 1 || corner == 3);
        int bottom = (corner >= 2);
        ctx->grabOppX = (right ? -0.5f : 0.5f) * ctx->grabWidth;
        ctx->grabOppY = (bottom ? 0.5f : -0.5f) * ctx->grabHeight;
        // A room's drag is measured from where the ray met that diagonal now.
        // A hand carrying it starts from the corner itself.
        ctx->grabRoomPercent = roomStyle > 0 ? roomScreenPercent(ctx, roomStyle) : 0;
        ctx->grabRoomReach = ctx->grabByGaze ? 1.0f : diagonalReach(ctx, u, v);
        ctx->grabMode = GRAB_RESIZE;
        return;
    }

    int h = ctx->grabHand;
    if (ctx->grabMode == GRAB_MOVE) {
        if (ctx->grabByGaze) {
            // Nothing to attach to, since the eyes did the pointing. The
            // picture slides by however far the hand has carried it.
            Vec3 travel = vecSub(gazeHandPos(ctx, aims, h), ctx->grabHandStart);
            Vec3 carry = gazeDragCarry(ctx, &ctx->grabRamp, travel, ctx->lastInputNs);
            ctx->screenPose.position.x = ctx->grabScreen.position.x + carry.x * ctx->grabScale;
            ctx->screenPose.position.y = ctx->grabScreen.position.y + carry.y * ctx->grabScale;
            ctx->screenPose.position.z = ctx->grabScreen.position.z + carry.z * ctx->grabScale;
        }
        else {
            // Where it goes is still the rigid attach: the offset from the
            // hand is carried round by the full hand turn, so the screen swings
            // with the same leverage it always did rather than sliding flat.
            XrQuaternionf turn = quatMul(aims[h].orientation,
                                         quatConj(ctx->grabAim.orientation));
            Vec3 offset = { ctx->grabScreen.position.x - ctx->grabAim.position.x,
                            ctx->grabScreen.position.y - ctx->grabAim.position.y,
                            ctx->grabScreen.position.z - ctx->grabAim.position.z };
            Vec3 moved = quatRotate(turn, offset);

            ctx->screenPose.position.x = aims[h].position.x + moved.x;
            ctx->screenPose.position.y = aims[h].position.y + moved.y;
            ctx->screenPose.position.z = aims[h].position.z + moved.z;
        }

        // Which way it faces does not. Inheriting the wrist tumbled the
        // picture on all three axes, so instead it keeps the tilt and roll it
        // was picked up with and turns to face the viewer from wherever it has
        // been dragged to.
        float hx = ctx->headPos.x - ctx->screenPose.position.x;
        float hz = ctx->headPos.z - ctx->screenPose.position.z;
        // Dragged directly over or under the head there is no sensible way to
        // face, and the yaw would spin on noise. Keep last frame's.
        if (hx * hx + hz * hz > 0.0025f) {
            ctx->screenPose.orientation = screenOrient(atan2f(hx, hz), ctx->grabPitch,
                                                       ctx->grabRoll);
        }
        return;
    }

    // Resize. Everything is measured against the pose the grab started from,
    // so growing the screen cannot feed back into where the ray lands on it.
    float reach;
    if (ctx->grabByGaze) {
        // The hand runs the corner out from where it was, along the screen's
        // own axes, rather than a ray landing on the plane
        Vec3 travel = vecSub(gazeHandPos(ctx, aims, h), ctx->grabHandStart);
        Vec3 carry = gazeDragCarry(ctx, &ctx->grabRamp, travel, ctx->lastInputNs);
        Vec3 xAxis = { 1.0f, 0.0f, 0.0f };
        Vec3 yAxis = { 0.0f, 1.0f, 0.0f };
        Vec3 right = quatRotate(ctx->grabScreen.orientation, xAxis);
        Vec3 up = quatRotate(ctx->grabScreen.orientation, yAxis);
        reach = diagonalReachAt(ctx, -ctx->grabOppX + vecDot(carry, right) * ctx->grabScale,
                                -ctx->grabOppY + vecDot(carry, up) * ctx->grabScale);
    }
    else {
        float u, v;
        if (!screenProject(aims[h], ctx->grabScreen, ctx->grabWidth, ctx->grabHeight,
                           ctx->grabRadius, curved, &u, &v)) {
            return;
        }
        reach = diagonalReach(ctx, u, v);
    }
    float scale = reach < 0.05f ? 0.05f : reach;

    int roomStyle = roomEffective(ctx);
    if (roomStyle > 0) {
        // In a room the drag sets how much of the room's screen the picture
        // fills, a quarter of it to all of it, in the whole percent the size
        // row shows, from where the ray was when the grip closed. The room
        // keeps the centre where it hangs it, and the share goes to the
        // preference once the hand lets go.
        int percent = roomResizePercent(ctx->grabRoomPercent, ctx->grabRoomReach, reach);
        if (percent != ctx->roomScreen[roomStyle]) {
            ctx->roomScreen[roomStyle] = percent;
            ctx->roomScreenUnsaved = roomStyle;
        }
        // Hung now rather than at the end of the frame, so the ray and the
        // bracket it ends on move with the picture
        applyRoomPlacement(ctx, roomStyle, (float)ctx->videoHeight / (float)ctx->videoWidth, 0);
        return;
    }

    float width = ctx->grabWidth * scale;
    if (width < SCREEN_MIN_WIDTH) width = SCREEN_MIN_WIDTH;
    if (width > SCREEN_MAX_WIDTH) width = SCREEN_MAX_WIDTH;

    // Keeping the arc the same shape rather than flattening as it grows
    ctx->screenRadius = ctx->grabRadius * (width / ctx->grabWidth);
    ctx->screenWidth = width;

    // The centre stays where it was, so the screen grows evenly about the spot
    // it was placed on rather than walking off towards one corner
    ctx->screenPose.position = ctx->grabScreen.position;
    ctx->screenPose.orientation = ctx->grabScreen.orientation;
}

// The press was meant for the thing that is open, not for the host behind it.
// Gaze has no button of its own, so swallowing a gaze press means swallowing
// the pinch that stood in for it.
static void swallowTrigger(XrCtx* ctx, int src) {
    if (src < 0 || src >= SRC_COUNT) {
        return;
    }
    ctx->triggerSwallowed[src] = 1;
    if (src == SRC_GAZE) {
        for (int h = 0; h < HAND_COUNT; h++) {
            if (ctx->triggerDown[h]) {
                ctx->triggerSwallowed[h] = 1;
            }
        }
    }
}

// Whether a hover is one of the pieces hung against the furniture's frame
// rather than the picture itself
static int onFurniture(int hover) {
    return hover == HOVER_BAR || hover == HOVER_ENVBUTTON || hover == HOVER_COGBUTTON
            || hover == HOVER_KBBUTTON || hover == HOVER_EXITBUTTON || hover == HOVER_LOCK
            || hover == HOVER_STEREOBUTTON;
}

// Where the ray lands on furniture rather than on the picture. The grid and the
// panels have planes of their own, and in a room the bar and its buttons sit
// on the stand in. Everything else sits on the screen.
static Vec3 furniturePoint(XrCtx* ctx, int hover, float u, float v, XrPosef screenPose,
                           float height, float radius, int curved) {
    if (furnitureOnStandIn(ctx) && onFurniture(hover)) {
        return screenPoint(u, v, standInPose(), STAND_IN_WIDTH_M, furnitureHeight(ctx),
                           0.0f, 0);
    }
    if (hover == HOVER_PICKER) {
        float pickW, pickH;
        XrPosef pose = pickerPose(ctx, &pickW, &pickH);
        return screenPoint(u, v, pose, pickW, pickH, 0.0f, 0);
    }
    if (hover == HOVER_COGPANEL) {
        return screenPoint(u, v, ctx->cogPose, ctx->cogW, ctx->cogH, 0.0f, 0);
    }
    if (hover == HOVER_KBPANEL) {
        return screenPoint(u, v, ctx->kbPose, ctx->kbW, ctx->kbH, 0.0f, 0);
    }
    if (hover == HOVER_EXITPROMPT) {
        return screenPoint(u, v, ctx->exitPose, ctx->exitW, ctx->exitH, 0.0f, 0);
    }
    return screenPoint(u, v, screenPose, ctx->screenWidth, height, radius, curved);
}

void destroyXrInput(XrCtx* ctx) {
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->handTrackers[h] != XR_NULL_HANDLE && ctx->pfnDestroyHandTracker != NULL) {
            ctx->pfnDestroyHandTracker(ctx->handTrackers[h]);
            ctx->handTrackers[h] = XR_NULL_HANDLE;
        }
    }
    for (int h = 0; h < SRC_COUNT; h++) {
        if (ctx->aimSpaces[h] != XR_NULL_HANDLE) {
            xrDestroySpace(ctx->aimSpaces[h]);
            ctx->aimSpaces[h] = XR_NULL_HANDLE;
        }
    }
    if (ctx->actionSet != XR_NULL_HANDLE) {
        // Takes its actions with it
        xrDestroyActionSet(ctx->actionSet);
        ctx->actionSet = XR_NULL_HANDLE;
    }
    ctx->inputReady = 0;
}

// Everything one pass over the sources works out before deciding what the
// ray is on, shared between the stages below
typedef struct {
    // The IN_ slots handed back to Java
    float* out;
    long now;
    float dt;
    // Into the frame the screen is in, which is the head's while it is head
    // locked and the local space's otherwise
    FrameXform xform;
    int roomOn;
    int curved;
    float height;
    float radius;
    // How big the picture's corner brackets are, 0 where it has none
    float cornerSide;
    XrPosef screenPose;
    // The head in that frame, which head locked is where it always is
    XrPosef headPose;
    int headValid;
    XrPosef aimPoses[SRC_COUNT];
    int aimValid[SRC_COUNT];
    float hitU[SRC_COUNT];
    float hitV[SRC_COUNT];
    int hovers[SRC_COUNT];
    int corners[SRC_COUNT];
    // Tracked separately from the hover, because the lock filter wipes the
    // hovers and this is what says which source to spare
    int atLock[SRC_COUNT];
    int moved;
    // The same per hand, for each controller's own clock
    int handMoved[HAND_COUNT];
    int pinching;
    // A hand that only pinches while the eyes point: aimed, but hovering
    // nothing, so it neither claims a press the eyes aimed nor draws a ray
    int pinchOnly[HAND_COUNT];
    // Each hand's grip and thumbstick this frame
    float grab[HAND_COUNT];
    XrVector2f stick[HAND_COUNT];
    // Whether the gaze was asked for this frame, and came back usable
    int gazeAsked;
    int gazeUsable;
    // The source doing the pointing, or -1, and what it is over
    int hand;
    int hover;
} InputFrame;

// Whether the eyes are what is doing the pointing. A controller in use takes
// it, and so do the hands while the eyes are missing.
static int gazePointing(XrCtx* ctx) {
    return ctx->eyeGaze && ctx->gazeEnabled && !ctx->controllerAwake
            && !ctx->gazeBridge.bridged;
}

// Whether the eyes would be pointing but for the bridge, which is when the
// gaze is still asked for: the bridge has to see them come back
static int gazeWanted(XrCtx* ctx) {
    return ctx->eyeGaze && ctx->gazeEnabled && !ctx->controllerAwake;
}

static int handInUse(XrCtx* ctx, int h) {
    return controllerInUse(ctx->profileKind[h], ctx->aimTracked[h], ctx->aimClock[h].awake);
}

// A drag the eyes started, which finishes as theirs
static int gazeHoldingPress(XrCtx* ctx) {
    return (ctx->grabMode != GRAB_NONE && (ctx->grabByGaze || ctx->grabHand == SRC_GAZE))
            || (ctx->cogDragSlider >= 0
                && (ctx->cogDragByGaze || ctx->cogDragHand == SRC_GAZE));
}

// Who points, settled once at the top of the frame off the last frame's
// clocks, so nothing in a frame disagrees about it
static void updateControllerAwake(XrCtx* ctx) {
    int awake = handInUse(ctx, HAND_LEFT) || handInUse(ctx, HAND_RIGHT);
    // A controller picked up in the middle of a drag the eyes are holding
    // would take the pointer away and leave the drag half done
    if (awake && !ctx->controllerAwake && gazeHoldingPress(ctx)) {
        awake = 0;
    }
    if (awake == ctx->controllerAwake) {
        return;
    }
    ctx->controllerAwake = awake;
    // Only worth saying where there are eyes to hand it back to
    if (ctx->eyeGaze && ctx->gazeEnabled) {
        LOGEV("pointer: %s", awake ? "controller" : ctx->gazeBridge.bridged ? "hands" : "eyes");
    }
}

// Anything the last frame left held. The bridge only switches with none, so a
// press always ends with the source that started it.
static int pressHeld(XrCtx* ctx) {
    return ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT]
            || ctx->grabMode != GRAB_NONE || ctx->cogDragSlider >= 0;
}

// The hands pointing for eyes gone GAZE_BRIDGE_SEC, and giving it back the
// moment usable eyes return. Settled at the top of the frame after the
// controllers, off the last frame's gaze, for the same reason.
static void updateGazeBridge(XrCtx* ctx) {
    float sec = 0.0f;
    int event = gazeBridgeUpdate(&ctx->gazeBridge, ctx->eyeGaze && ctx->gazeEnabled,
                                 ctx->controllerAwake,
                                 ctx->sessionState == XR_SESSION_STATE_FOCUSED,
                                 pressHeld(ctx), nowNs(), GAZE_BRIDGE_SEC, &sec);
    if (event == BRIDGE_ON) {
        LOGEV("gaze bridge on after %.1f s, the hands point until the eyes are back", sec);
    }
    else if (event == BRIDGE_OFF) {
        LOGEV("gaze bridge off after %.1f s", sec);
    }
}

// Whether a source may point at a panel or the grid: the ones the pointing
// source is picked from, so not a hand that only pinches, not a sleeping
// pointer, and not a controller lying down while the eyes point
static int canPoint(XrCtx* ctx, const InputFrame* f, int h) {
    if (!f->aimValid[h] || !ctx->pointerAwake) {
        return 0;
    }
    if (h >= HAND_COUNT) {
        return 1;
    }
    return !f->pinchOnly[h] && !(gazePointing(ctx) && !handInUse(ctx, h));
}

// Gaze has no button of its own, so a pinch from either hand clicks wherever
// the eyes have landed
static void gazeTrigger(XrCtx* ctx, const InputFrame* f) {
    if (f->aimValid[SRC_GAZE]) {
        ctx->triggerDown[SRC_GAZE] = ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT];
        ctx->triggerEdge[SRC_GAZE] = ctx->triggerEdge[HAND_LEFT] || ctx->triggerEdge[HAND_RIGHT];
        ctx->usingHands[SRC_GAZE] = 1;
    }
    else {
        ctx->triggerDown[SRC_GAZE] = 0;
        ctx->triggerEdge[SRC_GAZE] = 0;
        ctx->usingHands[SRC_GAZE] = 0;
    }
}

// Drops whatever the pointer was holding once it stops being watched
static void releaseInput(XrCtx* ctx, float* out) {
    ctx->buttonsDown = 0;
    ctx->beamVisible = 0;
    // Nothing is being read, so last frame's answer must not stand
    ctx->aimTracked[HAND_LEFT] = 0;
    ctx->aimTracked[HAND_RIGHT] = 0;
    // And the eyes are not being asked for, which is not their going missing
    gazeBridgeTrack(&ctx->gazeBridge, 0, 0, 0);
    if (ctx->grabMode != 0) {
        // Dropping focus mid grab has to count as letting go, or the
        // anchor is stale when focus comes back and the screen jumps
        ctx->grabMode = 0;
        ctx->grabByGaze = 0;
        ctx->poseDirty = 1;
    }
    if (ctx->cogDragSlider >= 0) {
        // Same for a slider: a drag must not survive the trigger it
        // was being held with going unwatched. Ending it here still
        // writes the value, since out is flushed on the way out.
        cogDragEnded(ctx, out);
    }
}

// The buttons along the bar and the padlock, claimed off what the hover test
// said about the same point. u and v are on the furniture's frame, which is
// the picture outside a room and the stand in inside one.
static int furnitureHover(XrCtx* ctx, InputFrame* f, int h, int hover, float u, float v) {
    float height = furnitureHeight(ctx);
    // The button reaches past the left end of the bar's zone, so it is tested
    // here rather than after a hand has been picked. Otherwise the part of it
    // outside that zone belongs to no hand at all.
    if ((hover == HOVER_NONE || hover == HOVER_BAR) && envButtonHit(ctx, u, v, height)) {
        hover = HOVER_ENVBUTTON;
    }
    // The cog is the same button on the other side of the bar, so it is
    // claimed the same way
    if ((hover == HOVER_NONE || hover == HOVER_BAR) && cogButtonHit(ctx, u, v, height)) {
        hover = HOVER_COGBUTTON;
    }
    // And the keyboard is one further out again, far enough out that it sits
    // past the right end of the bar's zone entirely. That is halo ground, so
    // like the padlock on the left it has to claim the halo back or the ray
    // never reaches it.
    if ((hover == HOVER_NONE || hover == HOVER_BAR || hover == HOVER_HALO)
            && kbButtonHit(ctx, u, v, height)) {
        hover = HOVER_KBBUTTON;
    }
    // The exit button is the same distance out on the left, so it sits past
    // that end of the bar's zone and has to claim the halo back the same way
    if ((hover == HOVER_NONE || hover == HOVER_BAR || hover == HOVER_HALO)
            && exitButtonHit(ctx, u, v, height)) {
        hover = HOVER_EXITBUTTON;
    }
    // The 3D switch, one further out than the keyboard, on the same halo
    // ground
    if ((hover == HOVER_NONE || hover == HOVER_BAR || hover == HOVER_HALO)
            && stereoButtonHit(ctx, u, v, height)) {
        hover = HOVER_STEREOBUTTON;
    }
    // Off the left edge, so the halo owns that ground until the padlock claims
    // it back
    if (ctx->handsEnabled && ctx->lockIconShown && hover != HOVER_ENVBUTTON
            && (hover == HOVER_NONE || hover == HOVER_HALO)
            && lockButtonHit(ctx, u, v, height)) {
        hover = HOVER_LOCK;
        f->atLock[h] = 1;
    }
    return hover;
}

// In a room the picture and the furniture are on two different planes: the
// picture on the room's wall, the bar and its buttons on the stand in in front
// of the seat. The picture keeps what lands on it, the furniture takes what
// lands on a piece of it, and anything else is the picture's margin or nothing.
// The hit is left in the coordinates of whichever of the two claimed it.
static void roomHover(XrCtx* ctx, InputFrame* f, int h) {
    float pu = 0.0f, pv = 0.0f;
    int picture = HOVER_NONE;
    if (screenProject(f->aimPoses[h], f->screenPose, ctx->screenWidth, f->height,
                      f->radius, f->curved, &pu, &pv)) {
        picture = hoverTest(pu, pv, ctx->screenWidth, f->height, f->cornerSide,
                            &f->corners[h]);
    }
    if (picture == HOVER_SCREEN || picture == HOVER_CORNER) {
        f->hovers[h] = picture;
        f->hitU[h] = pu;
        f->hitV[h] = pv;
        return;
    }

    float su, sv;
    float standW = STAND_IN_WIDTH_M;
    float standH = furnitureHeight(ctx);
    if (screenProject(f->aimPoses[h], standInPose(), standW, standH, 0.0f, 0, &su, &sv)) {
        int unused;
        int stand = hoverTest(su, sv, standW, standH, 0.0f, &unused);
        stand = furnitureHover(ctx, f, h, stand, su, sv);
        if (onFurniture(stand)) {
            f->hovers[h] = stand;
            f->hitU[h] = su;
            f->hitV[h] = sv;
            return;
        }
    }

    // The picture's own bar zone means nothing here, the stand in's does
    f->hovers[h] = picture == HOVER_BAR ? HOVER_HALO : picture;
    f->hitU[h] = pu;
    f->hitV[h] = pv;
}

// Where a source's ray lands: the keyboard, the picture and its furniture, or
// in a room the picture and the stand in
static void hoverSource(XrCtx* ctx, InputFrame* f, int h) {
    // The keyboard is not modal, but it does own the ground it covers: it
    // hangs in front of the bar, so a ray that lands on it must not reach the
    // picture or the furniture behind.
    float kbU, kbV;
    int onKeyboard = ctx->kbOpen
            && screenProject(f->aimPoses[h], ctx->kbPose, ctx->kbW, ctx->kbH, 0.0f, 0,
                             &kbU, &kbV)
            && kbU >= 0.0f && kbU <= 1.0f && kbV >= 0.0f && kbV <= 1.0f;
    if (onKeyboard) {
        f->hovers[h] = HOVER_KBPANEL;
        f->hitU[h] = kbU;
        f->hitV[h] = kbV;
    }
    else if (!f->roomOn) {
        if (screenProject(f->aimPoses[h], f->screenPose, ctx->screenWidth, f->height,
                          f->radius, f->curved, &f->hitU[h], &f->hitV[h])) {
            int hover = hoverTest(f->hitU[h], f->hitV[h], ctx->screenWidth, f->height,
                                  f->cornerSide, &f->corners[h]);
            f->hovers[h] = furnitureHover(ctx, f, h, hover, f->hitU[h], f->hitV[h]);
        }
    }
    else {
        roomHover(ctx, f, h);
    }
}

// How fast the head is turning in the world, for the guard on a drag the eyes
// started. Head locked, a still hand stays put in the room while the head
// turns, so the world is the frame that matters either way.
static void updateHeadTurn(XrCtx* ctx, InputFrame* f) {
    if (!f->xform.ok) {
        ctx->headTurnLastValid = 0;
        ctx->headTurnRate = 0.0f;
        return;
    }
    ctx->headTurnRate = ctx->headTurnLastValid
            ? turnRateDegS(f->xform.head.orientation, ctx->headTurnLast, f->dt) : 0.0f;
    ctx->headTurnLast = f->xform.head.orientation;
    ctx->headTurnLastValid = 1;
}

// Reads every source: the triggers, the aim poses and where each ray lands
static void readSources(XrCtx* ctx, InputFrame* f) {
    int gazeSpace = ctx->aimSpaces[SRC_GAZE] != XR_NULL_HANDLE;
    // The eyes point and the hands only pinch, which is the rule while gaze
    // is on and no controller is in use
    int gazeMode = gazePointing(ctx) && gazeSpace;
    // The bridge still asks for the gaze, since it has to see the eyes come
    // back, but gives it nothing to point with
    int gazeWatch = gazeWanted(ctx) && gazeSpace;
    for (int h = 0; h < SRC_COUNT; h++) {
        if (h < HAND_COUNT) {
            f->grab[h] = actionFloat(ctx, ctx->grabAction, h);
            f->stick[h] = actionVec2(ctx, ctx->scrollAction, h);
            int wasDown = ctx->triggerDown[h];
            float value = actionFloat(ctx, ctx->triggerAction, h);
            // The joints are read whatever is on the hand, since the ray and
            // the point a drag follows come out of them too
            int joints = jointPinching(ctx, h, &f->xform, &f->headPose, f->headValid, f->now);
            // A hand's pinch, however the runtime reports how hard it is, has
            // its own pair of thresholds rather than the trigger's
            int byHand = ctx->profileKind[h] != PROFILE_CONTROLLER;
            float on = byHand ? PINCH_VALUE_ON : PRESS_ON;
            float off = byHand ? PINCH_VALUE_OFF : PRESS_OFF;
            // Where the EXT profile's pinch value is bound and on the hand it
            // is the press on its own, and the runtime has decided already.
            // Otherwise a bound value or a measured pinch will do, held for
            // PINCH_HOLD_NS first. Runtimes that offer neither leave this at
            // rest, which is what a headset with nothing in its hands should
            // report.
            int byValue = byHand && ctx->extHandClick && ctx->onExtHands[h];
            int want = pressHysteresis(value, wasDown, on, off) || (!byValue && joints);
            if (byHand && !byValue) {
                ctx->triggerDown[h] = pinchHoldStep(&ctx->pinchWantNs[h], want, wasDown, f->now);
            }
            else {
                ctx->pinchWantNs[h] = 0;
                ctx->triggerDown[h] = want;
            }
            ctx->triggerEdge[h] = ctx->triggerDown[h] && !wasDown;

            // Diagnostics only. A press held from a pinch reads zero here, so
            // a hand click shows as a hold that never leaves 0.00.
            ctx->triggerValue[h] = value;
            if (ctx->triggerEdge[h]) {
                ctx->triggerHoldMin[h] = value;
                ctx->triggerDipFrames[h] = 0;
                ctx->triggerRetaps[h] = 0;
                ctx->triggerDipping[h] = 0;
            }
            else if (ctx->triggerDown[h]) {
                if (value < ctx->triggerHoldMin[h]) {
                    ctx->triggerHoldMin[h] = value;
                }
                if (value < PRESS_ON) {
                    ctx->triggerDipFrames[h]++;
                    ctx->triggerDipping[h] = 1;
                }
                else if (ctx->triggerDipping[h]) {
                    ctx->triggerRetaps[h]++;
                    ctx->triggerDipping[h] = 0;
                }
            }
        }
        else if (!gazeWatch) {
            // No eyes, or a controller is doing the pointing: the gaze ray is
            // not built at all, so it hovers nothing and the pinch that stands
            // in for its button is left to the hands' own rays
            continue;
        }

        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT
                | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        int ok = XR_SUCCEEDED(xrLocateSpace(ctx->aimSpaces[h], ctx->localSpace,
                                            ctx->predictedDisplayTime, &loc))
                && intoFrame(&f->xform, &loc.pose);
        int located = ok && (loc.locationFlags & needed) == needed;
        if (h == SRC_GAZE) {
            // Tracked as well as valid. Untracked takes the not located path
            // below and leaves the gaze invalid for the frame.
            located = ok && gazeUsable((unsigned)loc.locationFlags);
            f->gazeAsked = 1;
            f->gazeUsable = located;
        }
        else {
            // A hand the runtime has lost still reports a last known pose, and
            // taking that for a controller in use is what made the pointing
            // flap between it and the eyes
            ctx->aimTracked[h] = located && aimFullyTracked((unsigned)loc.locationFlags)
                    && actionPoseActive(ctx, ctx->aimAction, h);
        }
        int fromJoints = 0;
        if (!located) {
            // No controller and no pointer pose from the runtime, so the ray
            // built out of the joints stands in. This is what makes hand
            // pointing work on runtimes that refuse the hand profile.
            if (h < HAND_COUNT && ctx->handRayValid[h]) {
                loc.pose = ctx->handRay[h];
                fromJoints = 1;
            }
            else {
                // Filtering across a tracking gap would sweep the ray in
                // from wherever the hand was last seen
                if (h < HAND_COUNT) {
                    ctx->aimFilterPos[h][0].valid = 0;
                    ctx->aimFilterPos[h][1].valid = 0;
                    ctx->aimFilterPos[h][2].valid = 0;
                    ctx->aimFilterRot[h].valid = 0;
                }
                continue;
            }
        }
        // The eyes are back but the bridge keeps the pointer with the hands
        // until the pinches let go, so the pose goes no further
        if (h == SRC_GAZE && !gazeMode) {
            continue;
        }
        // Hands and controllers go through the pose filter. Gaze does not:
        // eyes move in saccades and the cursor is already smoothed downstream.
        if (h < HAND_COUNT) {
            XrPosef filtered = loc.pose;
            filtered.position.x = euroFilter(&ctx->aimFilterPos[h][0], filtered.position.x,
                                             f->dt, ctx->aimMinCutoff, ctx->aimBeta);
            filtered.position.y = euroFilter(&ctx->aimFilterPos[h][1], filtered.position.y,
                                             f->dt, ctx->aimMinCutoff, ctx->aimBeta);
            filtered.position.z = euroFilter(&ctx->aimFilterPos[h][2], filtered.position.z,
                                             f->dt, ctx->aimMinCutoff, ctx->aimBeta);
            filtered.orientation = euroFilterQuat(&ctx->aimFilterRot[h], filtered.orientation,
                                                  f->dt, ctx->aimMinCutoff, ctx->aimBeta);
            f->aimPoses[h] = filtered;
        }
        else {
            f->aimPoses[h] = loc.pose;
        }
        f->aimValid[h] = 1;

        if (ctx->poseSeen[h] && f->dt > 0.0f) {
            Vec3 now3 = { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z };
            Vec3 was3 = { ctx->lastAim[h].position.x, ctx->lastAim[h].position.y,
                          ctx->lastAim[h].position.z };
            Vec3 step = vecSub(now3, was3);
            float speed = sqrtf(step.x * step.x + step.y * step.y + step.z * step.z) / f->dt;

            // Angle between the two orientations, from the dot product of the
            // quaternions, which is half the rotation
            XrQuaternionf a = loc.pose.orientation, b = ctx->lastAim[h].orientation;
            float dot = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
            if (dot > 1.0f) {
                dot = 1.0f;
            }
            float turn = 2.0f * acosf(dot) / f->dt;

            // Hands and eyes are never still, so their motion says nothing
            // about intent and the gate would just hold the pointer on forever
            if (!ctx->usingHands[h] && h != SRC_GAZE
                    && (speed > POINTER_MOVE_SPEED || turn > POINTER_TURN_SPEED)) {
                f->moved = 1;
                f->handMoved[h] = 1;
            }
        }
        ctx->lastAim[h] = loc.pose;
        ctx->poseSeen[h] = 1;

        // A tracked hand's ray is a guess drawn from the shoulder through the
        // knuckle, so left pointing while the eyes do it would claim every
        // press they aimed and drag whatever it lay across
        if (gazeMode && h < HAND_COUNT && (ctx->usingHands[h] || fromJoints)) {
            f->pinchOnly[h] = 1;
            continue;
        }
        hoverSource(ctx, f, h);
    }
}

// Locks the hands out or lets them back in, from the padlock or the gesture
static void setHandsLocked(XrCtx* ctx, int locked, const char* from) {
    ctx->handsLocked = locked;
    LOGEV("hands %s by %s", locked ? "locked" : "unlocked", from);
    if (locked) {
        // Put the ray away and let go of anything held, so locking mid drag
        // does not leave the screen stuck to a hand or a button down on the
        // host
        ctx->pointerAwake = 0;
        ctx->buttonsDown = 0;
        ctx->stillFor = 0.0f;
        ctx->movingFor = 0.0f;
        if (ctx->grabMode != GRAB_NONE) {
            ctx->grabMode = GRAB_NONE;
            ctx->grabByGaze = 0;
            ctx->poseDirty = 1;
        }
    }
}

// The thumb to ring finger gesture, which turns the lock the way the padlock
// does and works whether the padlock is shown or not. Read before the lock is
// applied, since a locked hand has to be able to use it to get back.
static void updateLockGesture(XrCtx* ctx, InputFrame* f) {
    static const char* const REFUSED[] = {
        "", "the index tip is near the thumb", "the middle tip is near the thumb",
        "the hand is pressing", "the hand is gripping"
    };
    for (int h = 0; h < HAND_COUNT; h++) {
        if (!ctx->handsEnabled || ctx->profileKind[h] == PROFILE_CONTROLLER) {
            ringGateReset(&ctx->ringGate[h]);
            ctx->ringRefusalSaid[h] = RING_OK;
            continue;
        }
        // An index pinch or a grab under way is something else being done
        // with that hand, and in a fist the thumb is near every tip
        int busy = f->grab[h] > PRESS_ON ? RING_GRAB
                : (ctx->triggerDown[h] || ctx->pinchGate[h].down) ? RING_PRESSED : RING_OK;
        int refused = RING_OK;
        int fired = ringGateStep(&ctx->ringGate[h], ctx->ringTipsTracked[h], ctx->ringGap[h],
                                 ctx->indexGap[h], ctx->middleGap[h], busy, f->now, &refused);
        if (!ctx->ringGate[h].closed) {
            ctx->ringRefusalSaid[h] = RING_OK;
        }
        else if (refused != RING_OK && refused != ctx->ringRefusalSaid[h]) {
            ctx->ringRefusalSaid[h] = refused;
            LOGI("ring pinch on hand %d refused: %s", h, REFUSED[refused]);
        }
        if (fired) {
            setHandsLocked(ctx, !ctx->handsLocked, "the ring pinch");
            ctx->lockFlashNs = f->now;
        }
    }
}

// Keeps locked hands off everything but the padlock, and gives gaze the pinch
// it clicks with
static void applyHandLock(XrCtx* ctx, InputFrame* f) {
    // Locked hands reach the padlock and nothing else. Everything is dropped
    // at once, the aim as well as the pinch, so there is no ray to chase, no
    // click to land and no grab to start. Controllers are untouched: they
    // never had the problem, and one has to stay able to unlock.
    for (int h = 0; h < HAND_COUNT; h++) {
        if (!ctx->handsLocked || !ctx->usingHands[h] || f->atLock[h]) {
            continue;
        }
        f->hovers[h] = HOVER_NONE;
        f->aimValid[h] = 0;
        // The eyes on the padlock still get the pinch that presses it, since
        // a hand that only pinches has no ray of its own to reach it with
        if (!f->atLock[SRC_GAZE]) {
            ctx->triggerDown[h] = 0;
            ctx->triggerEdge[h] = 0;
        }
    }

    for (int h = 0; h < SRC_COUNT; h++) {
        if (!f->atLock[h]) {
            ctx->lockArmed[h] = 0;
        }
        else if (!ctx->triggerDown[h]) {
            ctx->lockArmed[h] = 1;
        }
    }

    gazeTrigger(ctx, f);
}

// Whether a button is down on this controller. Each is read on this hand's
// own path, since a press on the other controller says nothing about this one.
static int controllerPressed(XrCtx* ctx, const InputFrame* f, int h) {
    return ctx->triggerDown[h] || f->grab[h] > PRESS_ON || stickPushed(f->stick[h])
            || actionBool(ctx, ctx->rightClickAction, h)
            || actionBool(ctx, ctx->middleClickAction, h)
            || actionBool(ctx, ctx->toggleAction, h);
}

// The pointer's clock again, one per controller. The shared one is held on by
// the other hand and the eyes, so on its own it never retires a controller
// that was put down, and while the eyes point a controller has to be awake on
// this one to take the pointer off them.
static void updateControllerClocks(XrCtx* ctx, InputFrame* f) {
    int eyesPoint = ctx->eyeGaze && ctx->gazeEnabled;
    int swallowed = 0;
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->profileKind[h] != PROFILE_CONTROLLER) {
            controllerClockReset(&ctx->aimClock[h]);
            continue;
        }
        // Where the eyes point, movement alone never wakes a controller: one
        // lying on a lap and nudged took the pointing off them for five
        // seconds at a time, often with its ray on nothing
        int pressWakes = eyesPoint;
        int pressed = pressWakes && controllerPressed(ctx, f, h);
        int holding = ctx->triggerDown[h] || stickPushed(f->stick[h]);
        int swallow = 0;
        int event = controllerClockStep(&ctx->aimClock[h], f->dt, f->handMoved[h], pressed,
                                        holding, ctx->triggerDown[h], pressWakes,
                                        ctx->pointerSleepOn, ctx->pointerWake,
                                        ctx->pointerSleep, &swallow);
        if (event == CLOCK_PRESSED || event == CLOCK_PICKED_UP) {
            LOGI("controller %d %s, its ray is back", h,
                 event == CLOCK_PRESSED ? "pressed" : "picked up");
            // The shared pointer comes with it, or nothing would hover
            ctx->pointerAwake = 1;
            ctx->stillFor = 0.0f;
        }
        else if (event == CLOCK_PUT_DOWN) {
            LOGI("controller %d put down, its ray is gone", h);
        }
        // The press that woke it is held back for as long as it is held: its
        // ray was hidden, so there was nothing to aim it with
        if (swallow) {
            ctx->triggerDown[h] = 0;
            ctx->triggerEdge[h] = 0;
            swallowed = 1;
        }
    }
    // The eyes were already handed whatever trigger was down, so one held
    // back here has to come off them too
    if (swallowed && ctx->usingHands[SRC_GAZE]) {
        gazeTrigger(ctx, f);
    }
}

// Wakes the pointer on a pinch or a deliberate move and retires it once the
// controller has been still a while
static void updatePointerWake(XrCtx* ctx, InputFrame* f) {
    // A pinch is what a hand has instead of deliberate movement: it turns the
    // pointer on, and keeps it on for as long as pinches keep arriving. The
    // one that does the waking is swallowed rather than passed on as a click,
    // since the user was reaching for the pointer and not for the screen.
    for (int h = 0; h < SRC_COUNT; h++) {
        if (!ctx->usingHands[h]) {
            ctx->pinchSwallowed[h] = 0;
            continue;
        }
        // A pinch on the padlock is always meant as a press. The swallow is
        // there to keep a waking pinch off the host, and the padlock is not
        // the host, so charging the user a pinch for it buys nothing.
        if (f->atLock[h]) {
            ctx->pinchSwallowed[h] = 0;
            continue;
        }
        if (ctx->triggerDown[h]) {
            f->pinching = 1;
            if (!ctx->pointerAwake) {
                ctx->pointerAwake = 1;
                ctx->pinchSwallowed[h] = 1;
            }
        }
        else {
            ctx->pinchSwallowed[h] = 0;
        }
        if (ctx->pinchSwallowed[h]) {
            ctx->triggerDown[h] = 0;
            ctx->triggerEdge[h] = 0;
        }
    }

    // A press taken by the grid, the panel or the keyboard stays taken for as
    // long as it is held, and letting go is what hands the trigger back
    for (int h = 0; h < SRC_COUNT; h++) {
        if (!ctx->triggerDown[h]) {
            ctx->triggerSwallowed[h] = 0;
        }
    }

    // The thumbstick is as deliberate as a pinch: a controller held still
    // while a long page scrolled used to be retired mid scroll, and the stick
    // did nothing until it was waved about. A push wakes the pointer at once
    // and holds it up for the usual time after the last one.
    int stick = stickPushed(f->stick[HAND_LEFT]) || stickPushed(f->stick[HAND_RIGHT]);
    if (stick) {
        ctx->pointerAwake = 1;
    }

    // Deliberate movement wakes the pointer, a controller put down retires it
    if (f->pinching || stick) {
        // Only the pinch clock matters while hands are in charge
        ctx->stillFor = 0.0f;
        ctx->movingFor = 0.0f;
    }
    else if (f->moved) {
        ctx->movingFor += f->dt;
        ctx->stillFor = 0.0f;
        if (ctx->movingFor >= ctx->pointerWake) {
            ctx->pointerAwake = 1;
        }
    }
    else {
        ctx->stillFor += f->dt;
        ctx->movingFor = 0.0f;
        if (ctx->stillFor >= ctx->pointerSleep) {
            ctx->pointerAwake = 0;
        }
    }

    // With the pause switched off a controller in the hand keeps the pointer
    // up however still it is held, so the stick and the trigger always work.
    // Hands keep their own rule, since a resting hand points at the screen.
    if (!ctx->pointerSleepOn && (ctx->profileKind[HAND_LEFT] == PROFILE_CONTROLLER
                                 || ctx->profileKind[HAND_RIGHT] == PROFILE_CONTROLLER)) {
        ctx->pointerAwake = 1;
        ctx->stillFor = 0.0f;
    }

    updateControllerClocks(ctx, f);
}

// Chooses the source doing the pointing, and what it is over
static void pickPointingSource(XrCtx* ctx, InputFrame* f) {
    // The hand holding the trigger wins, so a drag is never stolen by the other
    // one drifting across the screen. Right hand otherwise.
    static const int order[SRC_COUNT] = { HAND_RIGHT, HAND_LEFT, SRC_GAZE };

    // While the eyes point, a controller has to be in use to take the pointer
    // off them. One put down goes on reporting a pose, and lying with its ray
    // across a button it lit the button and ate every pinch aimed elsewhere.
    if (gazePointing(ctx)) {
        for (int h = 0; h < HAND_COUNT; h++) {
            if (!handInUse(ctx, h)) {
                f->hovers[h] = HOVER_NONE;
            }
        }
    }

    f->hand = -1;
    for (int i = 0; i < SRC_COUNT; i++) {
        int h = order[i];
        if (f->hovers[h] == HOVER_SCREEN && ctx->triggerDown[h]) {
            f->hand = h;
            break;
        }
    }
    // A hand on something beats one merely near it, so a controller resting in
    // the margin never takes the pointer off the one being aimed
    for (int pass = 0; pass < 2 && f->hand < 0; pass++) {
        for (int i = 0; i < SRC_COUNT && f->hand < 0; i++) {
            int h = order[i];
            if (f->hovers[h] == HOVER_NONE || (pass == 0 && f->hovers[h] == HOVER_HALO)) {
                continue;
            }
            f->hand = h;
        }
    }

    // The padlock is reachable with the pointer asleep, because locking is
    // what put it to sleep and there would otherwise be no way back
    int reachingLock = f->hand >= 0 && f->atLock[f->hand];
    if (!ctx->pointerAwake && !reachingLock && ctx->grabMode == GRAB_NONE) {
        f->hand = -1;
        for (int h = 0; h < SRC_COUNT; h++) {
            f->hovers[h] = HOVER_NONE;
        }
    }

    f->hover = f->hand >= 0 ? f->hovers[f->hand] : HOVER_NONE;
    if (f->hover == HOVER_CORNER) {
        ctx->hoverCorner = f->corners[f->hand];
    }
}

// Puts every button and hover mark back to rest, for this frame to relight
// whichever the ray is on
static void clearHotState(XrCtx* ctx) {
    ctx->pickerHover = -1;
    ctx->envButtonHot = 0;
    ctx->cogButtonHot = 0;
    ctx->cogHoverSlider = -1;
    ctx->cogHoverCell = -1;
    ctx->lockHot = 0;
    ctx->pickerPick = -1;
    ctx->kbButtonHot = 0;
    ctx->kbHoverKey = -1;
    ctx->kbKeyDown = 0;
    ctx->exitButtonHot = 0;
    ctx->exitHoverZone = EXIT_ZONE_NONE;
    ctx->stereoButtonHot = 0;
}

// The picker is modal: while it is open the ray belongs to it and nothing
// reaches the picture behind
static void updatePicker(XrCtx* ctx, InputFrame* f) {
    f->hover = HOVER_PICKER;
    // Anything the hands were pointing at before belongs to the screen,
    // and reading those coordinates as grid coordinates would land the
    // ray somewhere it never was
    f->hand = -1;
    float pickW, pickH;
    XrPosef pose = pickerPose(ctx, &pickW, &pickH);
    for (int h = 0; h < SRC_COUNT; h++) {
        float pu, pv;
        if (!canPoint(ctx, f, h)) {
            continue;
        }
        if (!screenProject(f->aimPoses[h], pose, pickW, pickH, 0.0f, 0, &pu, &pv)) {
            continue;
        }
        if (pu < 0.0f || pu > 1.0f || pv < 0.0f || pv > 1.0f) {
            continue;
        }
        // Each band is a header strip over a row of cells. The strip is a
        // label and nothing else, so pointing at one is pointing at
        // nothing and a press there closes the grid like a press outside.
        int band = (int)(pv * PICKER_ROWS);
        if (band >= PICKER_ROWS) band = PICKER_ROWS - 1;
        float inBand = pv * PICKER_TEX_H - band * PICKER_BAND_PX;
        if (inBand < PICKER_HEADER_PX) {
            continue;
        }
        int col = (int)(pu * PICKER_COLS);
        if (col >= PICKER_COLS) col = PICKER_COLS - 1;
        // The grid is drawn full whatever shipped, so a cell past the last
        // real one is a blank tile: pointing at nothing, like a header strip
        int cell = band * PICKER_COLS + col;
        if (cell >= ctx->pickerCells) {
            continue;
        }
        ctx->pickerHover = cell;
        f->hand = h;
        f->hitU[h] = pu;
        f->hitV[h] = pv;

        if (ctx->triggerEdge[h]) {
            ctx->pickerPick = ctx->pickerHover;
            ctx->pickerChoice = ctx->pickerHover;
            ctx->pickerOpen = 0;
            swallowTrigger(ctx, h);
        }
        break;
    }

    // A press that lands on no cell, whether on a header strip or nowhere
    // near the grid at all, closes it
    if (ctx->pickerOpen && ctx->pickerHover < 0) {
        for (int h = 0; h < SRC_COUNT; h++) {
            if (ctx->triggerEdge[h]) {
                ctx->pickerOpen = 0;
                swallowTrigger(ctx, h);
            }
        }
    }
}

// A press on a slider. The eyes only choose the row, so a gaze press hands the
// drag to the hand that pinched and remembers where the thumb was put.
static void cogStartDrag(XrCtx* ctx, InputFrame* f, int h, int face, int row, float pu,
                         float pv) {
    ctx->cogDragByGaze = 0;
    ctx->cogDragHand = h;
    if (h == SRC_GAZE) {
        Vec3 target = screenPoint(pu, pv, ctx->cogPose, ctx->cogW, ctx->cogH, 0.0f, 0);
        int hand = gazeDragHand(ctx, f->aimPoses, f->aimValid, target,
                                &ctx->cogDragHandStart, &ctx->cogDragScale);
        if (hand < 0) {
            // No hand to carry it, so the press sets the value where it
            // landed and the drag ends on the next frame, which is what
            // writes the value
            ctx->cogDragHand = -1;
            ctx->cogDragSlider = row;
            ctx->cogDragFace = face;
            cogApplySlider(ctx, face, row, pu);
            return;
        }
        ctx->cogDragByGaze = 1;
        ctx->cogDragHand = hand;
        ctx->cogDragStartU = pu;
        dragRampStart(&ctx->cogDragRamp, f->now);
        LOGI("gaze drag: slider %d by hand %d, scale %.2f", row, hand, ctx->cogDragScale);
    }
    ctx->cogDragSlider = row;
    ctx->cogDragFace = face;
    // Jumps to where the press landed rather than waiting for the first bit
    // of movement
    cogApplySlider(ctx, face, row, pu);
}

// Modal in the same way the grid is, and against the pose frozen when
// it opened rather than wherever the screen has since been dragged to
static void updateCogPanel(XrCtx* ctx, InputFrame* f) {
    f->hover = HOVER_COGPANEL;
    f->hand = -1;
    int face = cogFace(ctx);

    // A drag keeps the hand that started it, and keeps it even once the
    // ray has wandered off the panel, so a slider can be run to either end
    // in one go. The display and Room tabs are cells apart from their tracks.
    if (ctx->cogDragSlider >= 0 && ctx->cogDragFace != face) {
        // A room came or went mid drag, which only a debug property can do,
        // and the row under the thumb is another row now
        cogDragEnded(ctx, f->out);
    }
    else if (ctx->cogDragSlider >= 0 && cogRowIsTrack(face, ctx->cogDragSlider)) {
        int h = ctx->cogDragHand;
        float pu = 0.0f, pv = 0.0f;
        int held = h >= 0 && ctx->triggerDown[h]
                && (f->aimValid[h] || (ctx->cogDragByGaze && pinchTracked(ctx, h)));
        if (held && ctx->cogDragByGaze) {
            // The eyes picked the row, the hand runs the thumb along it: its
            // travel across the panel's own width, past a dead zone, since at
            // the gearing a far panel asks for a shaking pinch would slide it
            Vec3 travel = vecSub(gazeHandPos(ctx, f->aimPoses, h), ctx->cogDragHandStart);
            Vec3 carry = gazeDragCarry(ctx, &ctx->cogDragRamp,
                                       dragDeadZone(travel, GAZE_DRAG_DEAD_M), f->now);
            Vec3 xAxis = { 1.0f, 0.0f, 0.0f };
            Vec3 right = quatRotate(ctx->cogPose.orientation, xAxis);
            pu = ctx->cogDragStartU + vecDot(carry, right) * ctx->cogDragScale / ctx->cogW;
            if (pu < 0.0f) pu = 0.0f;
            if (pu > 1.0f) pu = 1.0f;
            pv = cogRowV(face, ctx->cogDragSlider);
        }
        else if (held) {
            held = screenProject(f->aimPoses[h], ctx->cogPose, ctx->cogW, ctx->cogH,
                                 0.0f, 0, &pu, &pv);
        }
        if (held) {
            // Still the eyes' drag as far as the rest of the frame goes, so no
            // ray is drawn out of the hand that happens to be carrying it
            f->hand = ctx->cogDragByGaze ? SRC_GAZE : h;
            f->hitU[f->hand] = pu;
            f->hitV[f->hand] = pv;
            ctx->cogHoverSlider = ctx->cogDragSlider;
            cogApplySlider(ctx, face, ctx->cogDragSlider, pu);
        }
        else {
            cogDragEnded(ctx, f->out);
        }
    }

    for (int h = 0; f->hand < 0 && h < SRC_COUNT; h++) {
        float pu, pv;
        if (!canPoint(ctx, f, h)) {
            continue;
        }
        if (!screenProject(f->aimPoses[h], ctx->cogPose, ctx->cogW, ctx->cogH,
                           0.0f, 0, &pu, &pv)) {
            continue;
        }
        if (pu < 0.0f || pu > 1.0f || pv < 0.0f || pv > 1.0f) {
            continue;
        }
        f->hand = h;
        f->hitU[h] = pu;
        f->hitV[h] = pv;

        // The tab bar runs across the top, one even slot per tab. A press
        // up here changes tab and reaches nothing else.
        if (pv < COG_TAB_BAR_B) {
            if (ctx->triggerEdge[h]) {
                int t = (int)(pu * COG_TAB_COUNT);
                if (t >= COG_TAB_COUNT) t = COG_TAB_COUNT - 1;
                ctx->cogTab = t;
                ctx->cogDragSlider = -1;
                ctx->cogDragHand = -1;
                ctx->cogDragFace = -1;
                ctx->cogDragByGaze = 0;
            }
            break;
        }

        // A row that can do nothing here is drawn greyed, and the ray
        // passes over it as if it were not there
        int rowCount = cogTabRowCount(face);
        int row = -1;
        for (int s = 0; s < rowCount; s++) {
            if (!cogRowLive(ctx, face, s)) {
                continue;
            }
            if (fabsf(pv - cogRowV(face, s)) < cogRowHalf(face)) {
                row = s;
                break;
            }
        }

        if (row >= 0 && !cogRowIsTrack(face, row)) {
            // Cells, so a press picks one rather than starting a drag
            int cell = cogCellAt(pu, cogRowCells(face, row));
            ctx->cogHoverSlider = cell >= 0 ? row : -1;
            ctx->cogHoverCell = cell;
            if (cell >= 0 && ctx->triggerEdge[h]) {
                cogApplyCell(ctx, face, row, cell, f->out);
            }
            break;
        }

        // Sliders. The band reaches a little past both ends of the track,
        // since the thumb hangs over them.
        if (row >= 0 && (pu <= COG_TRACK_L - 0.04f || pu >= COG_TRACK_R + 0.04f)) {
            row = -1;
        }
        ctx->cogHoverSlider = row;

        // Only the screen and 3D tabs have a reset button under their rows
        int onReset = (face == COG_TAB_SCREEN || face == COG_TAB_3D)
                && pu >= COG_RESET_L && pu <= COG_RESET_R
                && pv >= COG_RESET_T && pv <= COG_RESET_B;
        if (onReset && ctx->triggerEdge[h] && face == COG_TAB_3D) {
            // The running model's own pair, handed down when the session
            // started, so the button works the same way whatever the
            // preferences were left on. Still allowed while stereo is off,
            // where it does no harm and keeps the button from being a dead
            // rectangle.
            ctx->panelSeparation = ctx->defaultSeparation;
            ctx->separationCurrent = ctx->defaultSeparation;
            ctx->convergence = ctx->defaultConvergence;
            f->out[IN_SETTING] = (float)SETTING_RESET_3D;
            f->out[IN_SETTING_VALUE] = 0.0f;
            LOGEV("3d settings reset from the panel to separation %d, convergence %d",
                  separationUnits(ctx->defaultSeparation),
                  (int)roundf(ctx->defaultConvergence * 100.0f));
        }
        else if (onReset && ctx->triggerEdge[h]) {
            // Hands the curve back to the preference and drops the
            // placement, which is all it takes: updatePlacement reseeds
            // from the preferences on the next frame and marks the pose
            // dirty itself, so the reset persists with nothing else to do.
            // The panel stays open so the jump is visible.
            ctx->panelCurve = -1.0f;
            ctx->placementValid = 0;
            LOGI("screen placement reset from the panel");
        }
        else if (row >= 0 && ctx->triggerEdge[h]) {
            cogStartDrag(ctx, f, h, face, row, pu, pv);
        }
    }

    // A press that lands off the panel closes it, which is also how the
    // cog button shuts what it opened. One inside that hits nothing is
    // swallowed, so a near miss does not put the panel away.
    if (f->hand < 0) {
        for (int h = 0; h < SRC_COUNT; h++) {
            if (ctx->triggerEdge[h]) {
                ctx->cogOpen = 0;
                swallowTrigger(ctx, h);
            }
        }
    }
}

// Modal like the grid and the panel, and against the pose frozen when
// it opened. Ending the stream is not something to do by accident, so
// nothing outside the prompt is reachable while it is up.
static void updateExitPrompt(XrCtx* ctx, InputFrame* f) {
    f->hover = HOVER_EXITPROMPT;
    f->hand = -1;

    for (int h = 0; h < SRC_COUNT; h++) {
        float pu, pv;
        if (!canPoint(ctx, f, h)) {
            continue;
        }
        if (!screenProject(f->aimPoses[h], ctx->exitPose, ctx->exitW, ctx->exitH,
                           0.0f, 0, &pu, &pv)) {
            continue;
        }
        if (pu < 0.0f || pu > 1.0f || pv < 0.0f || pv > 1.0f) {
            continue;
        }
        f->hand = h;
        f->hitU[h] = pu;
        f->hitV[h] = pv;
        ctx->exitHoverZone = exitPromptZone(pu, pv);

        if (ctx->triggerEdge[h]) {
            if (ctx->exitHoverZone == EXIT_ZONE_EXIT) {
                // Said once. Java takes the session down from here, and
                // the prompt closes either way so a refused exit leaves
                // the button usable.
                f->out[IN_EXIT] = 1.0f;
                LOGI("exit confirmed from the prompt");
            }
            ctx->exitConfirmOpen = 0;
            swallowTrigger(ctx, h);
        }
        break;
    }

    // A press anywhere off the sheet puts it away, the way one off the
    // grid closes that
    if (f->hand < 0) {
        for (int h = 0; h < SRC_COUNT; h++) {
            if (ctx->triggerEdge[h]) {
                ctx->exitConfirmOpen = 0;
                swallowTrigger(ctx, h);
            }
        }
    }
}

// Lights whichever piece of furniture the ray is on, and acts on a press there
static void updateFurniture(XrCtx* ctx, InputFrame* f) {
    if (f->hover == HOVER_ENVBUTTON) {
        ctx->envButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            ctx->pickerOpen = 1;
        }
    }
    else if (f->hover == HOVER_COGBUTTON) {
        ctx->cogButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            ctx->cogOpen = !ctx->cogOpen;
            if (ctx->cogOpen) {
                // Always opens on the first tab, so the button does the same
                // thing every time
                ctx->cogTab = COG_TAB_SCREEN;
                ctx->cogPose = cogPanelPose(ctx, &ctx->cogW, &ctx->cogH);
            }
        }
    }
    else if (f->hover == HOVER_KBBUTTON) {
        ctx->kbButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            ctx->kbOpen = !ctx->kbOpen;
            if (ctx->kbOpen) {
                // Always comes up in lowercase, so the first key is where the
                // eye expects it however it was left last time
                ctx->kbState = KB_STATE_LOWER;
                ctx->kbPose = kbPanelPose(ctx, &ctx->kbW, &ctx->kbH);
            }
            LOGI("keyboard %s", ctx->kbOpen ? "open" : "closed");
        }
    }
    else if (f->hover == HOVER_EXITBUTTON) {
        ctx->exitButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            ctx->exitConfirmOpen = 1;
            ctx->exitHoverZone = EXIT_ZONE_NONE;
            ctx->exitPose = exitPromptPose(ctx, &ctx->exitW, &ctx->exitH);
            // The press belonged to the button, not to the host behind it
            swallowTrigger(ctx, f->hand);
            LOGI("exit prompt open");
        }
    }
    else if (f->hover == HOVER_STEREOBUTTON) {
        ctx->stereoButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            setStereoLive(ctx, !ctx->stereoLive, "the bar button");
        }
    }
    else if (f->hover == HOVER_KBPANEL) {
        int key = kbKeyAt(ctx, f->hitU[f->hand], f->hitV[f->hand]);
        ctx->kbHoverKey = key;
        ctx->kbKeyDown = key >= 0 && ctx->triggerDown[f->hand];
        if (key >= 0 && ctx->triggerEdge[f->hand]) {
            int code = ctx->kbCodes[ctx->kbState][key];
            if (code == KB_CODE_SHIFT) {
                // Shift off the symbols page goes to the capitals rather than
                // back where it came from
                ctx->kbState = ctx->kbState == KB_STATE_UPPER
                        ? KB_STATE_LOWER : KB_STATE_UPPER;
            }
            else if (code == KB_CODE_SYMBOLS) {
                ctx->kbState = ctx->kbState == KB_STATE_SYMBOLS
                        ? KB_STATE_LOWER : KB_STATE_SYMBOLS;
            }
            else if (code == KB_CODE_HIDE) {
                ctx->kbOpen = 0;
                LOGI("keyboard closed");
            }
            else if (code > 0) {
                // One key a frame, which is as fast as anyone presses them
                f->out[IN_KEY] = (float)code;
                // Shift is one shot over the letters, the way a phone keyboard
                // behaves, and sticky over the punctuation row above them
                if (ctx->kbState == KB_STATE_UPPER && code >= 'A' && code <= 'Z') {
                    ctx->kbState = KB_STATE_LOWER;
                }
            }
        }
    }
    else if (f->hover == HOVER_LOCK) {
        ctx->lockHot = 1;
        if (ctx->triggerEdge[f->hand] && ctx->lockArmed[f->hand]) {
            ctx->lockArmed[f->hand] = 0;
            setHandsLocked(ctx, !ctx->handsLocked, "the padlock");
        }
    }
}

// A press that lands on nothing at all puts the keyboard away, the same way
// one off the grid or the settings panel closes those. Only empty ground
// counts: a press on the picture is a mouse click and stays one, and the
// furniture and the keys themselves keep their own meanings. Every source
// is checked rather than the one doing the pointing, so a second hand can
// dismiss it while the first is still on the screen.
static void dismissKeyboard(XrCtx* ctx, InputFrame* f) {
    if (ctx->kbOpen && !ctx->pickerOpen && !ctx->cogOpen && !ctx->exitConfirmOpen) {
        for (int h = 0; h < SRC_COUNT; h++) {
            if (!canPoint(ctx, f, h) || !ctx->triggerEdge[h]) {
                continue;
            }
            // The halo is the invisible fringe around the picture, so it reads
            // as empty space too
            if (f->hovers[h] == HOVER_NONE || f->hovers[h] == HOVER_HALO) {
                ctx->kbOpen = 0;
                swallowTrigger(ctx, h);
                LOGI("keyboard closed");
                break;
            }
        }
    }
}

// One line that says whether gaze is tracking, whether it is the thing
// doing the pointing, and whether a pinch is reaching us at all. Logged
// only when it changes, so it costs nothing while it sits still.
static void logInputSnapshot(XrCtx* ctx, InputFrame* f) {
    int snapshot = (f->aimValid[SRC_GAZE] ? 1 : 0) | (f->hand == SRC_GAZE ? 2 : 0)
            | ((ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT]) ? 4 : 0)
            | (ctx->pointerAwake ? 8 : 0);
    if (snapshot != ctx->lastSnapshot) {
        ctx->lastSnapshot = snapshot;
        LOGI("input: gaze tracked %d, pointing by gaze %d, pinch %d, awake %d",
             (snapshot & 1) != 0, (snapshot & 2) != 0, (snapshot & 4) != 0,
             (snapshot & 8) != 0);
    }
}

// Nothing goes to the host mid drag, and the ray ends on the handle
// being held rather than wherever it is now pointing
static void beamToHandle(XrCtx* ctx, InputFrame* f) {
    ctx->buttonsDown = 0;
    ctx->scrollCarry = 0.0f;
    ctx->filterU.valid = 0;
    ctx->filterV.valid = 0;

    if (f->headValid) {
        Vec3 local;
        local.z = 0.0f;
        if (ctx->grabMode == GRAB_MOVE) {
            local.x = 0.0f;
            local.y = -(f->height * 0.5f + (BAR_GAP_FRAC + BAR_HEIGHT_FRAC * 0.5f)
                        * ctx->screenWidth);
        }
        else {
            // The bracket being held sits a half bracket outside the
            // corner, so the ray has to end out there with it
            float side = cornerSide(ctx);
            local.x = (ctx->grabOppX > 0.0f ? -0.5f : 0.5f) * (ctx->screenWidth + side);
            local.y = (ctx->grabOppY > 0.0f ? -0.5f : 0.5f) * (f->height + side);
        }
        curveLocal(&local, f->radius, f->curved, NULL);
        Vec3 handle = quatRotate(f->screenPose.orientation, local);
        ctx->beamStart = f->aimPoses[ctx->grabHand].position;
        ctx->beamEnd.x = f->screenPose.position.x + handle.x;
        ctx->beamEnd.y = f->screenPose.position.y + handle.y;
        ctx->beamEnd.z = f->screenPose.position.z + handle.z;
        ctx->beamVisible = 1;
    }
}

// Ends the ray on the furniture under it, whichever plane that sits on
static void beamToFurniture(XrCtx* ctx, InputFrame* f) {
    Vec3 end = furniturePoint(ctx, f->hover, f->hitU[f->hand], f->hitV[f->hand],
                              f->screenPose, f->height, f->radius, f->curved);
    ctx->beamStart = f->aimPoses[f->hand].position;
    ctx->beamEnd.x = end.x;
    ctx->beamEnd.y = end.y;
    ctx->beamEnd.z = end.z;
    ctx->beamVisible = f->headValid;
}

// Smooths the hit point, hands it to Java and ends the ray on it
static void sendPointer(XrCtx* ctx, InputFrame* f, int hit) {
    if (!hit) {
        return;
    }
    // Filtering across a gap or a change of hands would slide the cursor
    // in from wherever it used to be
    if (f->hand != ctx->lastHand || f->now - ctx->lastHitNs > POINTER_RESET_NS) {
        ctx->filterU.valid = 0;
        ctx->filterV.valid = 0;
    }
    ctx->lastHand = f->hand;
    ctx->lastHitNs = f->now;

    float u = euroFilter(&ctx->filterU, f->hitU[f->hand], f->dt, ctx->pointerMinCutoff,
                         ctx->pointerBeta);
    float v = euroFilter(&ctx->filterV, f->hitV[f->hand], f->dt, ctx->pointerMinCutoff,
                         ctx->pointerBeta);
    f->out[IN_HIT] = 1.0f;
    f->out[IN_U] = u;
    f->out[IN_V] = v;

    // The ray is only drawn when it lands on something, which is what
    // makes a laser readable rather than a light show
    Vec3 endPoint = screenPoint(u, v, f->screenPose, ctx->screenWidth, f->height, f->radius,
                                f->curved);
    ctx->beamStart = f->aimPoses[f->hand].position;
    ctx->beamEnd.x = endPoint.x;
    ctx->beamEnd.y = endPoint.y;
    ctx->beamEnd.z = endPoint.z;
    ctx->beamVisible = f->headValid;
}

// Works out which host buttons are down, and logs each trigger transition
static void updateButtons(XrCtx* ctx, InputFrame* f, int hit) {
    int mask = 0;
    for (int h = 0; h < HAND_COUNT; h++) {
        // Per hand rather than either hand, so a trigger spent on the grid or a
        // panel is out of the count while the other one still clicks
        if (ctx->triggerDown[h] && !ctx->triggerSwallowed[h]) {
            mask |= VR_BUTTON_LEFT;
        }
    }
    if (actionBool(ctx, ctx->rightClickAction, -1)) {
        mask |= VR_BUTTON_RIGHT;
    }
    if (actionBool(ctx, ctx->middleClickAction, -1)) {
        mask |= VR_BUTTON_MIDDLE;
    }
    // A press only counts while aimed at the screen, but a release always
    // does, so walking the pointer off the edge mid drag still lets go
    ctx->buttonsDown = (ctx->buttonsDown & mask) | (hit ? mask : 0);
    f->out[IN_BUTTONS] = (float)ctx->buttonsDown;

    // Diagnostics for the click path, one line per transition, so an ordinary
    // click costs two. A release carries the low water mark of the analog
    // value and how many frames it spent under the press threshold, which is
    // what tells a genuine pair of taps from two that merged into one press.
    int rawNow = ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT];
    int leftNow = (ctx->buttonsDown & VR_BUTTON_LEFT) ? 1 : 0;
    if (rawNow != ctx->trigLogRaw || leftNow != ctx->trigLogLeft
            || ctx->triggerDown[HAND_LEFT] != ctx->trigLogDown[HAND_LEFT]
            || ctx->triggerDown[HAND_RIGHT] != ctx->trigLogDown[HAND_RIGHT]) {
        char rel[80];
        rel[0] = '\0';
        for (int h = 0; h < HAND_COUNT; h++) {
            if (ctx->trigLogDown[h] && !ctx->triggerDown[h]) {
                snprintf(rel, sizeof(rel),
                         " up hand=%d holdMin=%.2f dipFrames=%d retaps=%d",
                         h, ctx->triggerHoldMin[h], ctx->triggerDipFrames[h],
                         ctx->triggerRetaps[h]);
            }
        }
        LOGI("trig: raw=%d%d val=%.2f,%.2f swal=%d%d edge=%d%d hit=%d awake=%d left=%d%s",
             ctx->triggerDown[HAND_LEFT], ctx->triggerDown[HAND_RIGHT],
             ctx->triggerValue[HAND_LEFT], ctx->triggerValue[HAND_RIGHT],
             ctx->triggerSwallowed[HAND_LEFT], ctx->triggerSwallowed[HAND_RIGHT],
             ctx->triggerEdge[HAND_LEFT], ctx->triggerEdge[HAND_RIGHT],
             hit ? 1 : 0, ctx->pointerAwake, leftNow, rel);

        ctx->trigLogRaw = rawNow;
        ctx->trigLogLeft = leftNow;
        ctx->trigLogDown[HAND_LEFT] = ctx->triggerDown[HAND_LEFT];
        ctx->trigLogDown[HAND_RIGHT] = ctx->triggerDown[HAND_RIGHT];
    }
}

// Winds the thumbstick into scroll clicks while the pointer is on the picture.
// With the pause off it scrolls with the ray off the picture as well, wherever
// the host's cursor was left, the way a wheel does, as long as the ray is not
// on something of ours.
static void updateScroll(XrCtx* ctx, InputFrame* f, int hit) {
    XrVector2f stick = actionVec2(ctx, ctx->scrollAction, -1);
    int offPicture = !ctx->pointerSleepOn
            && (f->hover == HOVER_NONE || f->hover == HOVER_HALO);
    if ((hit || offPicture) && fabsf(stick.y) > SCROLL_DEADZONE) {
        float past = (fabsf(stick.y) - SCROLL_DEADZONE) / (1.0f - SCROLL_DEADZONE);
        ctx->scrollCarry += copysignf(past * SCROLL_CLICKS_PER_SEC * f->dt, stick.y);
    }
    else {
        ctx->scrollCarry = 0.0f;
    }
    float clicks = truncf(ctx->scrollCarry);
    ctx->scrollCarry -= clicks;
    f->out[IN_SCROLL] = clicks;
}

// Aimed at nothing at all, so the ray runs off into the room rather than
// blinking out. A laser that comes and goes is harder to aim than one that
// always shows where the hand is looking, so the only thing that retires it
// is the controller being put down.
static void beamIntoRoom(XrCtx* ctx, InputFrame* f) {
    if (!ctx->beamVisible && ctx->pointerAwake && f->headValid && !ctx->beamGaze) {
        int free = f->hand;
        if (free < 0) {
            // Never a hand that only pinches, nor a controller lying down
            // while the eyes point
            free = canPoint(ctx, f, HAND_RIGHT) ? HAND_RIGHT
                    : (canPoint(ctx, f, HAND_LEFT) ? HAND_LEFT : -1);
        }
        if (free >= 0) {
            Vec3 forward = { 0.0f, 0.0f, -1.0f };
            Vec3 d = quatRotate(f->aimPoses[free].orientation, forward);
            ctx->beamStart = f->aimPoses[free].position;
            ctx->beamEnd.x = ctx->beamStart.x + d.x * FREE_BEAM_M;
            ctx->beamEnd.y = ctx->beamStart.y + d.y * FREE_BEAM_M;
            ctx->beamEnd.z = ctx->beamStart.z + d.z * FREE_BEAM_M;
            ctx->beamVisible = 1;
            // No target, so no cursor. The dot is what says a click would
            // land somewhere.
            ctx->beamFree = 1;
        }
    }
}

// The head's yaw against the screen, which the virtual surround turns its
// speakers by so the sound stays with the picture. A screen locked to the head
// turns with it, so there the answer is always 0; a room hangs the picture on
// its wall whatever the lock says. A frame that cannot place the head keeps
// the last answer rather than snapping the sound back to the front.
static void updateAudioYaw(XrCtx* ctx, int headLocked) {
    if (headLocked && roomEffective(ctx) <= 0) {
        ctx->audioYaw = 0.0f;
        return;
    }
    if (!ctx->sessionRunning || !ctx->placementValid) {
        return;
    }
    XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
    if (XR_SUCCEEDED(xrLocateSpace(ctx->viewSpace, ctx->localSpace,
                                   ctx->predictedDisplayTime, &head))
            && (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
        ctx->audioYaw = yawBetween(head.pose.orientation, ctx->screenPose.orientation);
    }
}

// A room's size left by a corner goes to the preference once the grab is over,
// however it ended, on the first frame with the setting slot free. A room gone
// from under it in the meantime leaves nothing to write it to.
static void emitRoomScreen(XrCtx* ctx, float* out) {
    int style = ctx->roomScreenUnsaved;
    if (style == 0 || ctx->grabMode == GRAB_RESIZE || out[IN_SETTING] >= 0.0f) {
        return;
    }
    ctx->roomScreenUnsaved = 0;
    if (style != roomEffective(ctx)) {
        return;
    }
    out[IN_SETTING] = (float)SETTING_ROOM_SCREEN;
    out[IN_SETTING_VALUE] = (float)roomScreenPercent(ctx, style);
    LOGEV("room %d screen percent %d from a corner", style, roomScreenPercent(ctx, style));
}

// The slots read off state rather than written as things happen, filled last
// so they say what this frame left, then the lot to Java
static void handBack(JNIEnv* env, XrCtx* ctx, float* out, jfloatArray outArr) {
    int readouts[READOUT_VALUES] = { -1, -1, -1 };
    out[IN_SETTING_ROOM] = -1.0f;
    out[IN_STEREO] = 0.0f;
    if (ctx != NULL) {
        emitRoomScreen(ctx, out);
        // Every room keeps its own values, so a room setting says whose it is
        out[IN_SETTING_ROOM] = (float)roomCellForStyle(roomEffective(ctx));
        cogReadouts(ctx, readouts);
        out[IN_STEREO] = ctx->stereoMode != DEPTH_MODE_OFF && ctx->stereoLive ? 1.0f : 0.0f;
    }
    for (int i = 0; i < READOUT_VALUES; i++) {
        out[IN_READOUT + i] = (float)readouts[i];
    }
    (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
}

// Reads the controllers and works out where they are pointing on the screen.
// Java turns the result into host mouse events, so nothing here knows about
// the connection.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUpdateInput(JNIEnv* env, jobject thiz,
                                                              jlong handle, jfloat distance,
                                                              jfloat quadWidth, jfloat curvature,
                                                              jboolean headLocked,
                                                              jboolean pointerEnabled,
                                                              jboolean gazeEnabled,
                                                              jboolean lockIcon,
                                                              jboolean pointerSleep,
                                                              jfloatArray outArr) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    float out[IN_SLOTS];
    memset(out, 0, sizeof(out));
    if (ctx != NULL) {
        ctx->gazeEnabled = gazeEnabled;
        ctx->lockIconShown = lockIcon;
        ctx->pointerSleepOn = pointerSleep;
        // Before anything asks who is pointing, off the last frame's clocks and
        // gaze, which are the only ones there are until the sources are read
        updateControllerAwake(ctx);
        updateGazeBridge(ctx);
        ctx->prefCurvature = curvature;
        // Ahead of every early return below, none of which clear it, so the
        // sound follows the head with the pointer off or focus lost too
        updateAudioYaw(ctx, headLocked);
        out[IN_HEAD_YAW] = ctx->audioYaw;
    }
    // Zero is a real cell, so "nothing picked" has to be said explicitly. Every
    // early return below would otherwise read as a press on the first one. Same
    // for the setting id, and nothing clears it again once a row has written
    // one, so the early returns carry it out too.
    out[IN_PICKER_PICK] = -1.0f;
    out[IN_SETTING] = -1.0f;
    // Backspace is 8, so a zeroed slot would type one every frame
    out[IN_KEY] = -1.0f;

    // Anything held has to come back up when pointing stops, or the host is
    // left with a stuck button. Nothing is pointed at under the splash either:
    // it hides everything a press could land on.
    if (ctx == NULL || !ctx->inputReady || !pointerEnabled || !ctx->placementValid
            || ctx->sessionState != XR_SESSION_STATE_FOCUSED
            || ctx->splash.phase != SPLASH_GONE) {
        if (ctx != NULL) {
            releaseInput(ctx, out);
        }
        handBack(env, ctx, out, outArr);
        return;
    }

    XrActiveActionSet active;
    active.actionSet = ctx->actionSet;
    active.subactionPath = XR_NULL_PATH;

    XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(ctx->session, &sync))) {
        ctx->buttonsDown = 0;
        handBack(env, ctx, out, outArr);
        return;
    }

    int toggle = actionBool(ctx, ctx->toggleAction, -1);
    if (toggle && !ctx->togglePrev) {
        ctx->pointerOn = !ctx->pointerOn;
        LOGI("pointer %s", ctx->pointerOn ? "on" : "off");
    }
    ctx->togglePrev = toggle;
    out[IN_POINTER] = ctx->pointerOn ? 1.0f : 0.0f;

    InputFrame f;
    memset(&f, 0, sizeof(f));
    f.out = out;
    // Both of these have to agree with what endFrame submits, or the ray lands
    // somewhere other than where the picture is drawn. A room world locks it
    // and flattens it whatever the preference and the panel say.
    f.roomOn = roomEffective(ctx) > 0;
    f.xform.toHead = headLocked && !f.roomOn;
    f.height = ctx->screenWidth * (float)ctx->videoHeight / (float)ctx->videoWidth;
    f.curved = !f.roomOn && effectiveCurvature(ctx) > 0.01f && ctx->cylinderSupported;
    f.radius = ctx->screenRadius;
    f.cornerSide = cornerSide(ctx);
    f.screenPose = ctx->screenPose;

    f.now = nowNs();
    f.dt = ctx->lastInputNs != 0 ? (f.now - ctx->lastInputNs) / 1e9f : 0.0f;
    ctx->lastInputNs = f.now;
    if (f.dt > 0.1f) {
        f.dt = 0.1f;
    }

    // The head in the local space, the one locate every frame makes whether
    // the screen is head locked or not
    XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
    int headOk = XR_SUCCEEDED(xrLocateSpace(ctx->viewSpace, ctx->localSpace,
                                            ctx->predictedDisplayTime, &head));
    const XrSpaceLocationFlags placed = XR_SPACE_LOCATION_POSITION_VALID_BIT
            | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    f.xform.ok = headOk && (head.locationFlags & placed) == placed;
    f.xform.head = head.pose;
    if (f.xform.toHead) {
        // In its own frame the head is always at the origin looking down -z
        memset(&f.headPose, 0, sizeof(f.headPose));
        f.headPose.orientation.w = 1.0f;
        f.headValid = f.xform.ok;
    }
    else {
        f.headPose = head.pose;
        f.headValid = headOk && (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    }
    if (f.headValid) {
        ctx->headPos = f.headPose.position;
    }
    updateHeadTurn(ctx, &f);

    readSources(ctx, &f);
    gazeBridgeTrack(&ctx->gazeBridge, f.gazeAsked, f.gazeUsable, f.now);
    updateLockGesture(ctx, &f);
    applyHandLock(ctx, &f);
    updatePointerWake(ctx, &f);
    pickPointingSource(ctx, &f);
    clearHotState(ctx);
    if (ctx->pickerOpen) {
        updatePicker(ctx, &f);
    }
    else if (ctx->cogOpen) {
        updateCogPanel(ctx, &f);
    }
    else if (ctx->exitConfirmOpen) {
        updateExitPrompt(ctx, &f);
    }
    else {
        updateFurniture(ctx, &f);
    }
    dismissKeyboard(ctx, &f);
    logInputSnapshot(ctx, &f);

    // Where the handle is clear of the picture, so a trigger press there cannot
    // have been meant as a click
    int offPicture = f.hand >= 0 && (f.hitU[f.hand] < 0.0f || f.hitU[f.hand] > 1.0f
                                     || f.hitV[f.hand] < 0.0f || f.hitV[f.hand] > 1.0f);
    applyGrab(ctx, f.aimPoses, f.aimValid, f.grab, f.hand, f.hover, ctx->hoverCorner,
              offPicture, f.height, f.curved);
    f.screenPose = ctx->screenPose;
    f.height = ctx->screenWidth * (float)ctx->videoHeight / (float)ctx->videoWidth;
    f.radius = ctx->screenRadius;

    // A handle stays lit while it is being dragged, however far the ray has
    // wandered from it in the meantime
    if (ctx->grabMode == GRAB_MOVE) {
        ctx->hoverKind = HOVER_BAR;
    }
    else if (ctx->grabMode == GRAB_RESIZE) {
        ctx->hoverKind = HOVER_CORNER;
    }
    else {
        ctx->hoverKind = f.hover;
    }

    ctx->screenOrientation = f.screenPose.orientation;
    ctx->beamVisible = 0;
    ctx->beamFree = 0;
    // Eyes aim by looking, so a ray out of the face would be nonsense, and a
    // cursor riding on them shakes too much to be anything but a distraction.
    // Gaze draws nothing: the handle lighting up is the feedback.
    // A grab the eyes started counts as theirs though a hand carries it
    ctx->beamGaze = ctx->grabMode != GRAB_NONE
            ? (ctx->grabByGaze || ctx->grabHand == SRC_GAZE) : f.hand == SRC_GAZE;

    if (ctx->grabMode != GRAB_NONE) {
        beamToHandle(ctx, &f);
        writeInputPose(ctx, out);
        handBack(env, ctx, out, outArr);
        return;
    }

    if (!ctx->pointerOn) {
        // The ray still shows on the handles and the grid, so the screen can
        // be tidied and the environment changed with the mouse switched off
        if (f.hand >= 0 && f.hover != HOVER_NONE && f.hover != HOVER_SCREEN) {
            beamToFurniture(ctx, &f);
        }
        ctx->buttonsDown = 0;
        writeInputPose(ctx, out);
        handBack(env, ctx, out, outArr);
        return;
    }

    // The bar, the button and the picker all sit off the picture, so pointing
    // at them must not drag the host cursor to the edge
    int hit = (f.hover == HOVER_SCREEN || f.hover == HOVER_CORNER) && f.hand != SRC_GAZE;
    if ((f.hover == HOVER_BAR || f.hover == HOVER_ENVBUTTON || f.hover == HOVER_PICKER
            || f.hover == HOVER_LOCK || f.hover == HOVER_HALO || f.hover == HOVER_COGBUTTON
            || f.hover == HOVER_COGPANEL || f.hover == HOVER_KBBUTTON
            || f.hover == HOVER_KBPANEL || f.hover == HOVER_EXITBUTTON
            || f.hover == HOVER_EXITPROMPT || f.hover == HOVER_STEREOBUTTON)
            && f.headValid && f.hand >= 0) {
        beamToFurniture(ctx, &f);
    }
    sendPointer(ctx, &f, hit);
    updateButtons(ctx, &f, hit);
    updateScroll(ctx, &f, hit);
    beamIntoRoom(ctx, &f);

    writeInputPose(ctx, out);
    out[IN_PICKER_PICK] = (float)ctx->pickerPick;
    handBack(env, ctx, out, outArr);
}

// Puts back a placement saved from a previous session. Marking the sliders as
// already seen stops the first frame taking the screen straight back off it.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetScreenPose(JNIEnv* env, jobject thiz,
                                                                jlong handle, jfloatArray poseArr) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || poseArr == NULL) {
        return;
    }
    float p[POSE_VALUES];
    if ((*env)->GetArrayLength(env, poseArr) < POSE_VALUES) {
        return;
    }
    (*env)->GetFloatArrayRegion(env, poseArr, 0, POSE_VALUES, p);

    if (p[7] < SCREEN_MIN_WIDTH || p[7] > SCREEN_MAX_WIDTH || p[8] <= 0.0f) {
        LOGW("stored screen placement out of range, ignoring it");
        return;
    }
    // Anything below zero means the panel never set a curve, and anything else
    // out of range is not worth trusting either
    ctx->panelCurve = (p[9] >= 0.0f && p[9] <= 1.0f) ? p[9] : -1.0f;

    ctx->screenPose.position.x = p[0];
    ctx->screenPose.position.y = p[1];
    ctx->screenPose.position.z = p[2];
    ctx->screenPose.orientation.x = p[3];
    ctx->screenPose.orientation.y = p[4];
    ctx->screenPose.orientation.z = p[5];
    ctx->screenPose.orientation.w = p[6];
    ctx->screenPose.orientation = quatNorm(ctx->screenPose.orientation);
    ctx->screenWidth = p[7];
    ctx->screenRadius = p[8];
    ctx->placementValid = 1;
    ctx->sliderSeen = 0;
    LOGI("restored screen placement %.2f %.2f %.2f, %.2f m wide",
         p[0], p[1], p[2], p[7]);
}
