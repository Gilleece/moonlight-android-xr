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

// A picture asked to wrap further than a cylinder layer may goes just under a
// full turn, drawn smaller and in proportion, and the pointer finds it as drawn
static void testCylinderClamp(void) {
    const float turn = 2.0f * 3.14159265f;
    // Under the limit nothing changes
    CHECK_NEAR(cylinderAngle(3.0f, 3.55f), 3.0f / 3.55f, 1e-6);
    CHECK(cylinderFit(3.0f, 3.55f) == 1.0f);
    // The panel's nearest screen at full curve, 3 m wide, asks for 15 rad
    float angle = cylinderAngle(3.0f, 0.2f);
    CHECK(angle < turn);
    CHECK(angle > turn - 0.02f);
    CHECK(angle == CYLINDER_MAX_ANGLE);
    CHECK_NEAR(cylinderFit(3.0f, 0.2f), 0.2 * CYLINDER_MAX_ANGLE / 3.0, 1e-6);
    CHECK_NEAR(cylinderFit(3.0f, 0.2f), 0.418, 1e-3);
    // Nothing to divide by is still a cylinder a runtime takes
    CHECK(cylinderAngle(3.0f, 0.0f) == CYLINDER_MAX_ANGLE);
    CHECK(cylinderFit(0.0f, 0.2f) == 1.0f);

    // Every distance the panel reaches, every width and curve, the way the
    // panel works the radius out: always under a turn, and the arc kept
    // wherever it fits
    const float distances[] = { 0.2f, 0.3f, 0.5f, 1.0f, 2.0f, 3.0f, 8.0f };
    const float widths[] = { 0.8f, 1.5f, 3.0f, 5.0f, 8.0f };
    const float curves[] = { 0.02f, 0.25f, 0.5f, 0.75f, 1.0f };
    int over = 0, wrong = 0, held = 0;
    for (int i = 0; i < (int)(sizeof(distances) / sizeof(distances[0])); i++) {
        for (int j = 0; j < (int)(sizeof(widths) / sizeof(widths[0])); j++) {
            for (int k = 0; k < (int)(sizeof(curves) / sizeof(curves[0])); k++) {
                float r = distances[i] * (1.0f + 3.0f * (1.0f - curves[k]));
                float w = widths[j];
                float a = cylinderAngle(w, r);
                float fit = cylinderFit(w, r);
                if (!(a > 0.0f && a < turn) || !(fit > 0.0f && fit <= 1.0f)) {
                    over++;
                }
                // The arc drawn is the picture's width at the drawn scale
                if (fabsf(a * r - w * fit) > 1e-4f * w) {
                    wrong++;
                }
                if (w / r < CYLINDER_MAX_ANGLE) {
                    if (a != w / r || fit != 1.0f) wrong++;
                }
                else {
                    held++;
                }
            }
        }
    }
    CHECK(over == 0);
    CHECK(wrong == 0);
    CHECK(held > 10);

    // Points placed on the drawn picture and aimed at from the viewer, who
    // sits on the axis, come back where they were put, edges included
    const float width = 3.0f;
    const float height = 1.6875f;
    const float radius = 0.2f;
    XrPosef screen;
    screen.orientation = identity();
    screen.position.x = 0.0f;
    screen.position.y = 0.0f;
    screen.position.z = -radius;
    const float points[][2] = { { 0.5f, 0.5f }, { 0.1f, 0.2f }, { 0.9f, 0.8f }, { 0.0f, 0.0f },
                                { 1.0f, 1.0f } };
    for (size_t p = 0; p < sizeof(points) / sizeof(points[0]); p++) {
        Vec3 target = screenPoint(points[p][0], points[p][1], screen, width, height, radius, 1);
        // On the cylinder, about the viewer
        CHECK_NEAR(sqrtf(target.x * target.x + target.z * target.z), radius, 1e-5);
        // At the drawn height
        CHECK_NEAR(target.y, (0.5f - points[p][1]) * height * cylinderFit(width, radius), 1e-5);
        Vec3 dir = vecNorm(target);
        Vec3 minusZ = { 0.0f, 0.0f, -1.0f };
        Vec3 axis = vecCross(minusZ, dir);
        float turnTo = acosf(fmaxf(-1.0f, fminf(1.0f, vecDot(minusZ, dir))));
        XrPosef aim;
        aim.position.x = aim.position.y = aim.position.z = 0.0f;
        aim.orientation = vecDot(axis, axis) < 1e-12f ? identity()
                                                      : axisAngleQuat(vecNorm(axis), turnTo);
        float u = -1.0f, v = -1.0f;
        CHECK(screenProject(aim, screen, width, height, radius, 1, &u, &v));
        CHECK_NEAR(u, points[p][0], 1e-3);
        CHECK_NEAR(v, points[p][1], 1e-3);
    }
    // The drawn edges sit half the held angle round either way
    Vec3 right = screenPoint(1.0f, 0.5f, screen, width, height, radius, 1);
    CHECK_NEAR(atan2f(right.x, -right.z), 0.5f * CYLINDER_MAX_ANGLE, 1e-4);
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

// How far the head has turned from the screen, which the virtual surround
// turns its speakers by
static void testYawBetween(void) {
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    Vec3 right = { 1.0f, 0.0f, 0.0f };
    Vec3 forward = { 0.0f, 0.0f, -1.0f };
    const float DEG = 3.1415927f / 180.0f;

    // Facing the screen is no turn
    CHECK_NEAR(yawBetween(identity(), identity()), 0.0f, 1e-6);

    // Left is positive, right negative
    CHECK_NEAR(yawBetween(axisAngleQuat(up, HALF_PI), identity()), HALF_PI, 1e-5);
    CHECK_NEAR(yawBetween(axisAngleQuat(up, -30.0f * DEG), identity()), -30.0f * DEG, 1e-5);

    // It is measured from the screen, wherever the screen has been put
    XrQuaternionf both = axisAngleQuat(up, 0.8f);
    CHECK_NEAR(yawBetween(both, both), 0.0f, 1e-5);
    XrQuaternionf screenLeft = axisAngleQuat(up, 40.0f * DEG);
    CHECK_NEAR(yawBetween(identity(), screenLeft), -40.0f * DEG, 1e-5);

    // Across straight behind it goes the short way, 20 degrees and not 340
    XrQuaternionf screen = axisAngleQuat(up, 170.0f * DEG);
    XrQuaternionf head = axisAngleQuat(up, -170.0f * DEG);
    CHECK_NEAR(yawBetween(head, screen), 20.0f * DEG, 1e-4);
    CHECK_NEAR(yawBetween(screen, head), -20.0f * DEG, 1e-4);

    // Looking up or tilting the head is not a turn
    CHECK_NEAR(yawBetween(axisAngleQuat(right, 30.0f * DEG), identity()), 0.0f, 1e-5);
    CHECK_NEAR(yawBetween(axisAngleQuat(forward, 30.0f * DEG), identity()), 0.0f, 1e-5);
    // Nor does a turn change for a tilt on top of it
    XrQuaternionf turned = quatMul(axisAngleQuat(up, 25.0f * DEG),
                                   axisAngleQuat(right, -20.0f * DEG));
    CHECK_NEAR(yawBetween(turned, identity()), 25.0f * DEG, 1e-4);
}

// A head locked screen's input is located in the local space and moved into
// the head's frame: a controller held 40 cm in front of the head, turned and
// raised in the room, comes out 40 cm straight ahead, and the head itself at
// the origin
static void testPoseInFrame(void) {
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    Vec3 right = { 1.0f, 0.0f, 0.0f };
    XrPosef head;
    head.orientation = quatMul(axisAngleQuat(up, 1.2f), axisAngleQuat(right, -0.3f));
    head.position.x = 0.5f;
    head.position.y = 1.6f;
    head.position.z = -0.2f;

    // 40 cm along the head's own -z, aimed the way it looks
    Vec3 ahead = { 0.0f, 0.0f, -0.4f };
    Vec3 off = quatRotate(head.orientation, ahead);
    XrPosef aim;
    aim.orientation = head.orientation;
    aim.position.x = head.position.x + off.x;
    aim.position.y = head.position.y + off.y;
    aim.position.z = head.position.z + off.z;

    XrPosef seen = poseInFrame(head, aim);
    CHECK_NEAR(seen.position.x, 0.0, 1e-5);
    CHECK_NEAR(seen.position.y, 0.0, 1e-5);
    CHECK_NEAR(seen.position.z, -0.4, 1e-5);
    CHECK_NEAR(fabsf(seen.orientation.w), 1.0, 1e-5);

    XrPosef self = poseInFrame(head, head);
    CHECK_NEAR(self.position.x, 0.0, 1e-6);
    CHECK_NEAR(self.position.z, 0.0, 1e-6);
    CHECK_NEAR(fabsf(self.orientation.w), 1.0, 1e-6);

    // An identity frame changes nothing
    XrPosef origin = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
    XrPosef same = poseInFrame(origin, aim);
    CHECK_NEAR(same.position.x, aim.position.x, 1e-6);
    CHECK_NEAR(same.position.y, aim.position.y, 1e-6);
    CHECK_NEAR(same.orientation.y, aim.orientation.y, 1e-6);

    // And a ray in the head's frame lands where the same ray in the room does
    XrPosef screen = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -3.0f } };
    float u = 0.0f, v = 0.0f;
    CHECK(screenProject(seen, screen, 3.0f, 1.6875f, 0.0f, 0, &u, &v));
    CHECK_NEAR(u, 0.5, 1e-5);
    CHECK_NEAR(v, 0.5, 1e-5);
}

// The same yaw, then pitch, then roll the screen's orientation is built from
static XrQuaternionf screenAngles(float yaw, float pitch, float roll) {
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    Vec3 right = { 1.0f, 0.0f, 0.0f };
    Vec3 fwd = { 0.0f, 0.0f, 1.0f };
    XrQuaternionf q = quatMul(axisAngleQuat(up, yaw), axisAngleQuat(right, -pitch));
    return quatNorm(quatMul(q, axisAngleQuat(fwd, roll)));
}

// A quaternion and its negation are the same turn
static double quatAgreement(XrQuaternionf a, XrQuaternionf b) {
    return fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
}

static void testRecentre(void) {
    // Swung 40 degrees to the left, 2 m out and 0.3 m up, facing the viewer,
    // tipped back 10 degrees and rolled 5
    float turn = 40.0f * (float)M_PI / 180.0f;
    float tilt = 10.0f * (float)M_PI / 180.0f;
    float roll = 5.0f * (float)M_PI / 180.0f;
    XrPosef swung;
    swung.position.x = -2.0f * sinf(turn);
    swung.position.y = 0.3f;
    swung.position.z = -2.0f * cosf(turn);
    swung.orientation = screenAngles(turn, tilt, roll);
    XrPosef back = poseRecentred(swung);
    CHECK_NEAR(back.position.x, 0.0, 1e-6);
    CHECK_NEAR(back.position.y, 0.3, 1e-6);
    CHECK_NEAR(back.position.z, -2.0, 1e-5);
    // Straight ahead, with the tilt and the roll it had
    CHECK_NEAR(quatAgreement(back.orientation, screenAngles(0.0f, tilt, roll)), 1.0, 1e-5);
    float before = sqrtf(swung.position.x * swung.position.x + swung.position.y * swung.position.y
                         + swung.position.z * swung.position.z);
    float after = sqrtf(back.position.x * back.position.x + back.position.y * back.position.y
                        + back.position.z * back.position.z);
    CHECK_NEAR(after, before, 1e-5);

    // Already straight ahead, so nothing moves
    XrPosef ahead = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.5f } };
    XrPosef same = poseRecentred(ahead);
    CHECK_NEAR(same.position.x, 0.0, 1e-6);
    CHECK_NEAR(same.position.z, -1.5, 1e-6);
    CHECK_NEAR(quatAgreement(same.orientation, ahead.orientation), 1.0, 1e-6);

    // Left behind the viewer by the old forward, it comes round to the front
    XrPosef behind;
    behind.position.x = 0.0f;
    behind.position.y = -0.2f;
    behind.position.z = 2.5f;
    behind.orientation = screenAngles((float)M_PI, 0.0f, 0.0f);
    XrPosef front = poseRecentred(behind);
    CHECK_NEAR(front.position.x, 0.0, 1e-6);
    CHECK_NEAR(front.position.y, -0.2, 1e-6);
    CHECK_NEAR(front.position.z, -2.5, 1e-5);
    CHECK_NEAR(quatAgreement(front.orientation, ahead.orientation), 1.0, 1e-5);

    // Carried off to the side without turning, it still ends square to the
    // viewer, at the distance it was across the floor
    XrPosef carried = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 0.1f, -1.0f } };
    XrPosef squared = poseRecentred(carried);
    CHECK_NEAR(squared.position.x, 0.0, 1e-6);
    CHECK_NEAR(squared.position.y, 0.1, 1e-6);
    CHECK_NEAR(squared.position.z, -sqrt(2.0), 1e-5);
    CHECK_NEAR(quatAgreement(squared.orientation, ahead.orientation), 1.0, 1e-6);
}

int main(void) {
    testVectors();
    testQuaternions();
    testEuroFilter();
    testMatrices();
    testScreenRoundTrip(0);
    testScreenRoundTrip(1);
    testCylinderClamp();
    testCurveLocal();
    testYawBetween();
    testPoseInFrame();
    testRecentre();
    return checksDone("xr_math");
}
