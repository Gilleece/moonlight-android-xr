// The controllers as they are drawn, see xr_controller.h
#include "xr_controller.h"
#include "xr_gate.h"

int raySwitchOn(int settingOn, int flipped) {
    return (settingOn != 0) != (flipped != 0);
}

int rayFlipFor(int settingOn, int wantOn) {
    return (settingOn != 0) != (wantOn != 0);
}

int rayDrawn(int settingOn, int flipped, int panelOpen) {
    return panelOpen != 0 || raySwitchOn(settingOn, flipped);
}

float pointerDotSize(float distance) {
    float size = distance * (PTR_DOT_SIZE_M / PTR_DOT_REF_M);
    return size < PTR_DOT_MIN_M ? PTR_DOT_MIN_M : (size > PTR_DOT_MAX_M ? PTR_DOT_MAX_M : size);
}

int controllerModelShown(int settingOn, int passthrough, int kind, int poseActive,
                         unsigned flags) {
    const unsigned needed = GATE_POSITION_VALID | GATE_ORIENTATION_VALID
            | GATE_ORIENTATION_TRACKED;
    return settingOn && !passthrough && kind == PROFILE_CONTROLLER && poseActive
            && (flags & needed) == needed;
}

void controllerModelMatrix(XrPosef grip, int leftHand, float* out16) {
    Vec3 ex = { leftHand ? -1.0f : 1.0f, 0.0f, 0.0f };
    Vec3 ey = { 0.0f, 1.0f, 0.0f };
    Vec3 ez = { 0.0f, 0.0f, 1.0f };
    Vec3 cols[3] = { quatRotate(grip.orientation, ex), quatRotate(grip.orientation, ey),
                     quatRotate(grip.orientation, ez) };
    for (int c = 0; c < 3; c++) {
        out16[c * 4 + 0] = cols[c].x;
        out16[c * 4 + 1] = cols[c].y;
        out16[c * 4 + 2] = cols[c].z;
        out16[c * 4 + 3] = 0.0f;
    }
    out16[12] = grip.position.x;
    out16[13] = grip.position.y;
    out16[14] = grip.position.z;
    out16[15] = 1.0f;
}

Vec3 controllerModelPoint(XrPosef grip, int leftHand, Vec3 local) {
    if (leftHand) {
        local.x = -local.x;
    }
    Vec3 turned = quatRotate(grip.orientation, local);
    Vec3 out = { turned.x + grip.position.x, turned.y + grip.position.y,
                 turned.z + grip.position.z };
    return out;
}
