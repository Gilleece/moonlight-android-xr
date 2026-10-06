// The controllers as drawn: when the ray's beam goes up, when a controller's
// model is drawn, where the model goes for each hand, and how wide the cursor
// dot is at each distance
#include "check.h"
#include "xr_controller.h"
#include "xr_gate.h"

static void testRaySwitch(void) {
    // The setting is where a session starts, and each flip turns it over
    CHECK(raySwitchOn(1, 0) == 1);
    CHECK(raySwitchOn(1, 1) == 0);
    CHECK(raySwitchOn(0, 0) == 0);
    CHECK(raySwitchOn(0, 1) == 1);
    // Any non zero reads as set
    CHECK(raySwitchOn(5, 0) == 1);
    CHECK(raySwitchOn(5, 7) == 0);

    // The flip a row's cell asks for lands the switch on that cell, from
    // either setting
    for (int setting = 0; setting < 2; setting++) {
        for (int want = 0; want < 2; want++) {
            CHECK(raySwitchOn(setting, rayFlipFor(setting, want)) == want);
        }
    }
    // Asking for what is already in force asks for no flip
    CHECK(rayFlipFor(1, 1) == 0);
    CHECK(rayFlipFor(0, 0) == 0);
}

static void testRayDrawn(void) {
    // Every combination, against the rule written out: the beam shows while
    // the switch is on, and while any panel is up whatever the switch says
    for (int setting = 0; setting < 2; setting++) {
        for (int flipped = 0; flipped < 2; flipped++) {
            for (int panel = 0; panel < 2; panel++) {
                int on = setting != flipped;
                CHECK(rayDrawn(setting, flipped, panel) == (on || panel));
            }
        }
    }

    // A session started with the setting off: hidden, back while the cog is
    // open, hidden again once it closes
    CHECK(!rayDrawn(0, 0, 0));
    CHECK(rayDrawn(0, 0, 1));
    CHECK(!rayDrawn(0, 0, 0));
    // The bar button turns it on for the session, and a panel changes nothing
    CHECK(rayDrawn(0, 1, 0));
    CHECK(rayDrawn(0, 1, 1));
    // Started on and switched off from the bar, the same as the setting off
    CHECK(!rayDrawn(1, 1, 0));
    CHECK(rayDrawn(1, 1, 1));
}

static void testModelShown(void) {
    const unsigned located = GATE_POSITION_VALID | GATE_ORIENTATION_VALID
            | GATE_ORIENTATION_TRACKED;
    const unsigned all = located | GATE_POSITION_TRACKED;
    // A controller held in view
    CHECK(controllerModelShown(1, 0, PROFILE_CONTROLLER, 1, all));
    // Out of the cameras for a moment: the position is the runtime's guess,
    // and the model stays with it
    CHECK(controllerModelShown(1, 0, PROFILE_CONTROLLER, 1, located));
    // Each condition on its own takes it away: the setting, passthrough,
    // hands or nothing on the hand, the grip not live, the orientation
    // untracked or either part not placed at all
    CHECK(!controllerModelShown(0, 0, PROFILE_CONTROLLER, 1, all));
    CHECK(!controllerModelShown(1, 1, PROFILE_CONTROLLER, 1, all));
    CHECK(!controllerModelShown(1, 0, PROFILE_HANDS, 1, all));
    CHECK(!controllerModelShown(1, 0, PROFILE_NONE, 1, all));
    CHECK(!controllerModelShown(1, 0, PROFILE_UNKNOWN, 1, all));
    CHECK(!controllerModelShown(1, 0, PROFILE_CONTROLLER, 0, all));
    CHECK(!controllerModelShown(1, 0, PROFILE_CONTROLLER, 1,
                                GATE_POSITION_VALID | GATE_ORIENTATION_VALID));
    CHECK(!controllerModelShown(1, 0, PROFILE_CONTROLLER, 1,
                                GATE_ORIENTATION_VALID | GATE_ORIENTATION_TRACKED));
    CHECK(!controllerModelShown(1, 0, PROFILE_CONTROLLER, 1,
                                GATE_POSITION_VALID | GATE_ORIENTATION_TRACKED));
    CHECK(!controllerModelShown(1, 0, PROFILE_CONTROLLER, 1, 0));
}

// A column major matrix times a point
static Vec3 apply(const float* m, Vec3 p) {
    Vec3 r = { m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
               m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
               m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] };
    return r;
}

static float det3(const float* m) {
    return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2])
            + m[8] * (m[1] * m[6] - m[5] * m[2]);
}

static XrPosef pose(float x, float y, float z, XrQuaternionf q) {
    XrPosef p;
    p.orientation = q;
    p.position.x = x;
    p.position.y = y;
    p.position.z = z;
    return p;
}

static void testModelMatrix(void) {
    float m[16];
    XrQuaternionf none = { 0.0f, 0.0f, 0.0f, 1.0f };

    // At the origin unturned, the right hand is the model as authored and the
    // left its mirror image in x, and nothing else changes
    controllerModelMatrix(pose(0, 0, 0, none), 0, m);
    for (int i = 0; i < 16; i++) {
        CHECK_NEAR(m[i], i % 5 == 0 ? 1.0 : 0.0, 1e-6);
    }
    controllerModelMatrix(pose(0, 0, 0, none), 1, m);
    for (int i = 0; i < 16; i++) {
        CHECK_NEAR(m[i], i == 0 ? -1.0 : (i % 5 == 0 ? 1.0 : 0.0), 1e-6);
    }

    // The grip's position is the last column and nothing else
    controllerModelMatrix(pose(0.2f, -0.3f, -0.5f, none), 0, m);
    CHECK_NEAR(m[12], 0.2, 1e-6);
    CHECK_NEAR(m[13], -0.3, 1e-6);
    CHECK_NEAR(m[14], -0.5, 1e-6);
    CHECK_NEAR(m[15], 1.0, 1e-6);
    CHECK_NEAR(m[3], 0.0, 1e-6);
    CHECK_NEAR(m[7], 0.0, 1e-6);
    CHECK_NEAR(m[11], 0.0, 1e-6);

    // A quarter turn to the left about up takes the model's right to straight
    // ahead and its forward to the left, as quatRotate has them
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    XrQuaternionf quarter = axisAngleQuat(up, (float)M_PI * 0.5f);
    controllerModelMatrix(pose(0, 0, 0, quarter), 0, m);
    Vec3 right = { 1.0f, 0.0f, 0.0f };
    Vec3 forward = { 0.0f, 0.0f, -1.0f };
    Vec3 r = apply(m, right);
    Vec3 f = apply(m, forward);
    CHECK_NEAR(r.x, 0.0, 1e-6);
    CHECK_NEAR(r.z, -1.0, 1e-6);
    CHECK_NEAR(f.x, -1.0, 1e-6);
    CHECK_NEAR(f.z, 0.0, 1e-6);

    // Any grip: the matrix and the point worked without it agree, for both
    // hands, on points across the model, and the left hand's point is the
    // right hand's for the point mirrored in x
    Vec3 axis = vecNorm((Vec3){ 0.3f, -0.8f, 0.5f });
    XrPosef grip = pose(-0.12f, 1.05f, -0.4f, axisAngleQuat(axis, 2.1f));
    Vec3 points[] = { { 0.0f, 0.0f, 0.0f }, { 0.006f, 0.093f, -0.024f },
                      { 0.0f, -0.055f, 0.022f }, { -0.008f, 0.054f, -0.016f },
                      { 0.03f, -0.01f, -0.06f } };
    for (int hand = 0; hand < 2; hand++) {
        controllerModelMatrix(grip, hand, m);
        for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
            Vec3 a = apply(m, points[i]);
            Vec3 b = controllerModelPoint(grip, hand, points[i]);
            CHECK_NEAR(a.x, b.x, 1e-6);
            CHECK_NEAR(a.y, b.y, 1e-6);
            CHECK_NEAR(a.z, b.z, 1e-6);
            Vec3 mirrored = { -points[i].x, points[i].y, points[i].z };
            Vec3 c = controllerModelPoint(grip, !hand, mirrored);
            CHECK_NEAR(b.x, c.x, 1e-6);
            CHECK_NEAR(b.y, c.y, 1e-6);
            CHECK_NEAR(b.z, c.z, 1e-6);
        }
        // A turn keeps its handedness and the left hand's mirror flips it,
        // with every column still a unit length apart and square to the rest
        CHECK_NEAR(det3(m), hand ? -1.0 : 1.0, 1e-5);
        for (int a = 0; a < 3; a++) {
            for (int b = 0; b < 3; b++) {
                float dot = m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1]
                        + m[a * 4 + 2] * m[b * 4 + 2];
                CHECK_NEAR(dot, a == b ? 1.0 : 0.0, 1e-5);
            }
        }
    }

    // The points that lie on the grip's own centre plane are where both hands
    // put them, and the two hands' models are each other's reflection there
    controllerModelMatrix(grip, 0, m);
    float left[16];
    controllerModelMatrix(grip, 1, left);
    Vec3 onPlane = { 0.0f, 0.04f, -0.03f };
    Vec3 a = apply(m, onPlane);
    Vec3 b = apply(left, onPlane);
    CHECK_NEAR(a.x, b.x, 1e-6);
    CHECK_NEAR(a.y, b.y, 1e-6);
    CHECK_NEAR(a.z, b.z, 1e-6);
    // Only the first column differs, and by its sign
    for (int i = 0; i < 16; i++) {
        CHECK_NEAR(left[i], i < 3 ? -m[i] : m[i], 1e-6);
    }
}

// The angle the dot fills, seen from distance metres
static double dotAngle(float distance) {
    return 2.0 * atan(0.5 * pointerDotSize(distance) / distance);
}

static void testDotSize(void) {
    // The default screen, 3 m off, keeps the 2.2 cm it always had
    CHECK_NEAR(pointerDotSize(3.0f), 0.022, 1e-6);
    // A metre away it is a third of that and looks the same size, and so it
    // does on Synthwave's wall 14 m off and the Grand Cinema's further still
    CHECK_NEAR(pointerDotSize(1.0f), 0.022 / 3.0, 1e-6);
    CHECK_NEAR(pointerDotSize(14.0f), 0.022 * 14.0 / 3.0, 1e-6);
    CHECK_NEAR(dotAngle(1.0f), dotAngle(3.0f), 1e-6);
    CHECK_NEAR(dotAngle(14.0f), dotAngle(3.0f), 1e-6);
    CHECK_NEAR(dotAngle(16.0f), dotAngle(3.0f), 1e-6);
    // Everywhere between the floor and the ceiling the angle holds
    float floorAt = PTR_DOT_MIN_M * PTR_DOT_REF_M / PTR_DOT_SIZE_M;
    float ceilingAt = PTR_DOT_MAX_M * PTR_DOT_REF_M / PTR_DOT_SIZE_M;
    CHECK(floorAt < 1.0f && ceilingAt > 16.0f);
    int off = 0;
    float last = 0.0f;
    for (float d = 0.05f; d < 40.0f; d += 0.05f) {
        float size = pointerDotSize(d);
        if (size < last) off++;
        last = size;
        if (size < PTR_DOT_MIN_M || size > PTR_DOT_MAX_M) off++;
        if (d > floorAt + 0.01f && d < ceilingAt - 0.01f
                && fabs(dotAngle(d) - dotAngle(3.0f)) > 1e-5) {
            off++;
        }
    }
    CHECK(off == 0);
    // Up close it stops at the floor rather than shrinking to a speck, far
    // off at the ceiling, and nothing measured is the floor
    CHECK(pointerDotSize(0.2f) == PTR_DOT_MIN_M);
    CHECK(pointerDotSize(100.0f) == PTR_DOT_MAX_M);
    CHECK(pointerDotSize(0.0f) == PTR_DOT_MIN_M);
    CHECK(pointerDotSize(-1.0f) == PTR_DOT_MIN_M);
}

int main(void) {
    testRaySwitch();
    testRayDrawn();
    testModelShown();
    testModelMatrix();
    testDotSize();
    return checksDone("xr_controller");
}
