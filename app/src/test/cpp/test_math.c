// The vector, quaternion, filter and screen geometry maths, checked against
// answers that can be worked out by hand
#include "check.h"
#include "xr_math.h"

static const float HALF_PI = 1.5707963f;

static XrQuaternionf identity(void) {
    XrQuaternionf q = { 0.0f, 0.0f, 0.0f, 1.0f };
    return q;
}

static void testVectors(void) {
    Vec3 a = { 1.0f, 2.0f, 3.0f };
    Vec3 b = { 0.5f, 0.5f, 0.5f };
    Vec3 d = vecSub(a, b);
    CHECK_NEAR(d.x, 0.5f, 1e-6);
    CHECK_NEAR(d.y, 1.5f, 1e-6);
    CHECK_NEAR(d.z, 2.5f, 1e-6);

    Vec3 x = { 1.0f, 0.0f, 0.0f };
    Vec3 y = { 0.0f, 1.0f, 0.0f };
    Vec3 z = vecCross(x, y);
    CHECK_NEAR(z.x, 0.0f, 1e-6);
    CHECK_NEAR(z.y, 0.0f, 1e-6);
    CHECK_NEAR(z.z, 1.0f, 1e-6);
    CHECK_NEAR(vecDot(a, b), 3.0f, 1e-6);

    Vec3 n = vecNorm(a);
    CHECK_NEAR(sqrtf(vecDot(n, n)), 1.0f, 1e-6);
    Vec3 zero = { 0.0f, 0.0f, 0.0f };
    Vec3 nz = vecNorm(zero);
    CHECK(nz.x == 0.0f && nz.y == 0.0f && nz.z == 0.0f);
}

static void testQuaternions(void) {
    // A quarter turn about up takes forward, which is -z, to the left
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    Vec3 forward = { 0.0f, 0.0f, -1.0f };
    Vec3 turned = quatRotate(axisAngleQuat(up, HALF_PI), forward);
    CHECK_NEAR(turned.x, -1.0f, 1e-5);
    CHECK_NEAR(turned.y, 0.0f, 1e-5);
    CHECK_NEAR(turned.z, 0.0f, 1e-5);

    // Turning and turning back is nothing at all
    XrQuaternionf q = axisAngleQuat(up, 0.7f);
    XrQuaternionf back = quatMul(q, quatConj(q));
    CHECK_NEAR(back.w, 1.0f, 1e-5);
    CHECK_NEAR(back.x, 0.0f, 1e-5);

    XrQuaternionf stretched = { 0.0f, 0.0f, 0.0f, 4.0f };
    XrQuaternionf unit = quatNorm(stretched);
    CHECK_NEAR(unit.w, 1.0f, 1e-6);
    XrQuaternionf nothing = { 0.0f, 0.0f, 0.0f, 0.0f };
    XrQuaternionf safe = quatNorm(nothing);
    CHECK_NEAR(safe.w, 1.0f, 1e-6);

    // The identity basis is the identity rotation, and a rotated basis comes
    // back as the rotation that made it
    Vec3 bx = { 1.0f, 0.0f, 0.0f };
    Vec3 by = { 0.0f, 1.0f, 0.0f };
    Vec3 bz = { 0.0f, 0.0f, 1.0f };
    XrQuaternionf fromBasis = quatFromBasis(bx, by, bz);
    CHECK_NEAR(fromBasis.w, 1.0f, 1e-6);
    XrQuaternionf rot = axisAngleQuat(up, 0.4f);
    XrQuaternionf rebuilt = quatFromBasis(quatRotate(rot, bx), quatRotate(rot, by),
                                          quatRotate(rot, bz));
    CHECK_NEAR(rebuilt.y, rot.y, 1e-5);
    CHECK_NEAR(rebuilt.w, rot.w, 1e-5);
}

static void testEuroFilter(void) {
    EuroState s = { 0 };
    // The first sample passes straight through
    CHECK_NEAR(euroFilter(&s, 3.0f, 0.01f, 1.0f, 0.0f), 3.0f, 1e-6);
    // A still input settles on itself
    float v = 0.0f;
    for (int i = 0; i < 500; i++) {
        v = euroFilter(&s, 5.0f, 0.01f, 1.0f, 0.0f);
    }
    CHECK_NEAR(v, 5.0f, 1e-3);
    // And a jump is smoothed rather than followed in one step
    float jumped = euroFilter(&s, 50.0f, 0.01f, 1.0f, 0.0f);
    CHECK(jumped > 5.0f && jumped < 50.0f);

    EuroQuatState qs = { 0 };
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    XrQuaternionf target = axisAngleQuat(up, 1.0f);
    XrQuaternionf out = identity();
    for (int i = 0; i < 500; i++) {
        out = euroFilterQuat(&qs, target, 0.01f, 1.0f, 0.0f);
    }
    CHECK_NEAR(out.y, target.y, 1e-3);
    CHECK_NEAR(out.w, target.w, 1e-3);
}

static void testMatrices(void) {
    float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float m[16];
    for (int i = 0; i < 16; i++) {
        m[i] = (float)i;
    }
    float out[16];
    matMul(out, id, m);
    for (int i = 0; i < 16; i++) {
        CHECK_NEAR(out[i], m[i], 1e-6);
    }

    // An identity pose sees the world as it is
    XrPosef pose;
    pose.orientation = identity();
    pose.position.x = pose.position.y = pose.position.z = 0.0f;
    float view[16];
    viewFromPose(view, pose);
    for (int i = 0; i < 16; i++) {
        CHECK_NEAR(view[i], id[i], 1e-6);
    }

    // A symmetric frustum has no off centre terms
    XrFovf fov = { -0.5f, 0.5f, 0.4f, -0.4f };
    float proj[16];
    projectionFromFov(proj, fov, 0.1f, 10.0f);
    CHECK_NEAR(proj[8], 0.0f, 1e-6);
    CHECK_NEAR(proj[9], 0.0f, 1e-6);
    CHECK_NEAR(proj[11], -1.0f, 1e-6);
}

// A point placed on the screen and aimed at from the origin projects back to
// where it was put, on the flat quad and on the cylinder alike
static void testScreenRoundTrip(int curved) {
    const float width = 3.0f;
    const float height = 1.6875f;
    const float radius = 6.0f;
    XrPosef screen;
    screen.orientation = identity();
    screen.position.x = 0.0f;
    screen.position.y = 0.0f;
    screen.position.z = -3.0f;

    const float points[][2] = { { 0.5f, 0.5f }, { 0.1f, 0.2f }, { 0.9f, 0.8f }, { 0.0f, 1.0f } };
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        Vec3 target = screenPoint(points[i][0], points[i][1], screen, width, height,
                                  radius, curved);
        // A pose that looks down -z is turned to face the target
        Vec3 dir = vecNorm(target);
        Vec3 minusZ = { 0.0f, 0.0f, -1.0f };
        Vec3 axis = vecCross(minusZ, dir);
        float angle = acosf(vecDot(minusZ, dir));
        XrPosef aim;
        aim.position.x = aim.position.y = aim.position.z = 0.0f;
        aim.orientation = angle < 1e-6f ? identity() : axisAngleQuat(vecNorm(axis), angle);

        float u = -1.0f, v = -1.0f;
        CHECK(screenProject(aim, screen, width, height, radius, curved, &u, &v));
        CHECK_NEAR(u, points[i][0], 1e-3);
        CHECK_NEAR(v, points[i][1], 1e-3);
    }

    // Aimed away from the screen the flat quad is missed outright. The viewer
    // is inside the cylinder, so that is always met somewhere, and the hit
    // lands far enough outside the picture for the hover tests to reject it.
    XrPosef away;
    away.position.x = away.position.y = away.position.z = 0.0f;
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    away.orientation = axisAngleQuat(up, 3.1415927f);
    float u = 0.5f, v = 0.5f;
    int hit = screenProject(away, screen, width, height, radius, curved, &u, &v);
    if (curved) {
        CHECK(hit && (u < -1.0f || u > 2.0f));
    }
    else {
        CHECK(!hit);
    }
}

static void testCurveLocal(void) {
    // Flat leaves everything alone
    Vec3 flat = { 1.0f, 0.5f, 0.02f };
    float yaw = 1.0f;
    curveLocal(&flat, 6.0f, 0, &yaw);
    CHECK_NEAR(flat.x, 1.0f, 1e-6);
    CHECK_NEAR(flat.z, 0.02f, 1e-6);
    CHECK_NEAR(yaw, 0.0f, 1e-6);

    // On the cylinder a point proud of the surface stays that far from the
    // axis, and the middle of the arc does not move
    Vec3 middle = { 0.0f, 0.0f, 0.01f };
    curveLocal(&middle, 6.0f, 1, &yaw);
    CHECK_NEAR(middle.x, 0.0f, 1e-6);
    CHECK_NEAR(middle.z, 0.01f, 1e-6);
    Vec3 edge = { 1.5f, 0.0f, 0.01f };
    curveLocal(&edge, 6.0f, 1, &yaw);
    float fromAxis = sqrtf(edge.x * edge.x + (edge.z - 6.0f) * (edge.z - 6.0f));
    CHECK_NEAR(fromAxis, 6.0f - 0.01f, 1e-4);
    CHECK_NEAR(yaw, -1.5f / 6.0f, 1e-6);
}

int main(void) {
    testVectors();
    testQuaternions();
    testEuroFilter();
    testMatrices();
    testScreenRoundTrip(0);
    testScreenRoundTrip(1);
    testCurveLocal();
    return checksDone("xr_math");
}
