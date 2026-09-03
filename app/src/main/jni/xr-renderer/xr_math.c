#include <string.h>

#include "xr_math.h"

Vec3 vecSub(Vec3 a, Vec3 b) {
    Vec3 r = { a.x - b.x, a.y - b.y, a.z - b.z };
    return r;
}

XrQuaternionf quatConj(XrQuaternionf q) {
    XrQuaternionf r = { -q.x, -q.y, -q.z, q.w };
    return r;
}

XrQuaternionf quatMul(XrQuaternionf a, XrQuaternionf b) {
    XrQuaternionf r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

// Repeated products drift off the unit sphere and the compositor is entitled
// to reject that
XrQuaternionf quatNorm(XrQuaternionf q) {
    float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-6f) {
        XrQuaternionf id = { 0.0f, 0.0f, 0.0f, 1.0f };
        return id;
    }
    q.x /= len;
    q.y /= len;
    q.z /= len;
    q.w /= len;
    return q;
}

// Axis assumed normalised, which anything that came out of quatRotate on a
// unit vector already is
XrQuaternionf axisAngleQuat(Vec3 axis, float angle) {
    float half = angle * 0.5f;
    float s = sinf(half);
    XrQuaternionf q = { axis.x * s, axis.y * s, axis.z * s, cosf(half) };
    return q;
}

Vec3 quatRotate(XrQuaternionf q, Vec3 v) {
    // v + w * (2 * cross(q.xyz, v)) + cross(q.xyz, 2 * cross(q.xyz, v))
    Vec3 u = { q.x, q.y, q.z };
    Vec3 t = { 2.0f * (u.y * v.z - u.z * v.y),
               2.0f * (u.z * v.x - u.x * v.z),
               2.0f * (u.x * v.y - u.y * v.x) };
    Vec3 r = { v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
               v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
               v.z + q.w * t.z + (u.x * t.y - u.y * t.x) };
    return r;
}

static float euroAlpha(float cutoff, float dt) {
    float tau = 1.0f / (2.0f * (float)M_PI * cutoff);
    return 1.0f / (1.0f + tau / dt);
}

float euroFilter(EuroState* s, float x, float dt, float minCutoff, float beta) {
    if (!s->valid || dt <= 0.0f) {
        s->valid = 1;
        s->x = x;
        s->dx = 0.0f;
        return x;
    }
    float dx = (x - s->x) / dt;
    s->dx += euroAlpha(POINTER_D_CUTOFF, dt) * (dx - s->dx);
    float cutoff = minCutoff + beta * fabsf(s->dx);
    s->x += euroAlpha(cutoff, dt) * (x - s->x);
    return s->x;
}

XrQuaternionf euroFilterQuat(EuroQuatState* s, XrQuaternionf q, float dt,
                                    float minCutoff, float beta) {
    if (!s->valid || dt <= 0.0f) {
        s->valid = 1;
        s->q = q;
        s->dAngle = 0.0f;
        return q;
    }
    // Quaternions cover every rotation twice, so take the near side
    float dot = s->q.x * q.x + s->q.y * q.y + s->q.z * q.z + s->q.w * q.w;
    if (dot < 0.0f) {
        q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w;
        dot = -dot;
    }
    if (dot > 1.0f) {
        dot = 1.0f;
    }
    float speed = 2.0f * acosf(dot) / dt;
    s->dAngle += euroAlpha(POINTER_D_CUTOFF, dt) * (speed - s->dAngle);
    float cutoff = minCutoff + beta * s->dAngle;
    float a = euroAlpha(cutoff, dt);
    XrQuaternionf r = {
        s->q.x + a * (q.x - s->q.x),
        s->q.y + a * (q.y - s->q.y),
        s->q.z + a * (q.z - s->q.z),
        s->q.w + a * (q.w - s->q.w),
    };
    float len = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (len > 0.0f) {
        r.x /= len; r.y /= len; r.z /= len; r.w /= len;
    }
    s->q = r;
    return r;
}

// Rotation whose local axes are the three given unit vectors. Used to stand a
// quad layer up along the beam while keeping its face toward the viewer.
XrQuaternionf quatFromBasis(Vec3 x, Vec3 y, Vec3 z) {
    float m[3][3] = {
        { x.x, y.x, z.x },
        { x.y, y.y, z.y },
        { x.z, y.z, z.z },
    };
    float trace = m[0][0] + m[1][1] + m[2][2];
    XrQuaternionf q;
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m[2][1] - m[1][2]) / s;
        q.y = (m[0][2] - m[2][0]) / s;
        q.z = (m[1][0] - m[0][1]) / s;
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q.w = (m[2][1] - m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    }
    else if (m[1][1] > m[2][2]) {
        float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q.w = (m[0][2] - m[2][0]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m[1][2] + m[2][1]) / s;
    }
    else {
        float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q.w = (m[1][0] - m[0][1]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25f * s;
    }
    return q;
}

Vec3 vecNorm(Vec3 v) {
    float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-6f) {
        Vec3 zero = { 0.0f, 0.0f, 0.0f };
        return zero;
    }
    Vec3 r = { v.x / len, v.y / len, v.z / len };
    return r;
}

Vec3 vecCross(Vec3 a, Vec3 b) {
    Vec3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return r;
}

float vecDot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Matrices are column major, the order GL takes them in, and out = a * b
void matMul(float* out, const float* a, const float* b) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            out[col * 4 + row] = a[row] * b[col * 4]
                    + a[4 + row] * b[col * 4 + 1]
                    + a[8 + row] * b[col * 4 + 2]
                    + a[12 + row] * b[col * 4 + 3];
        }
    }
}

// The runtime hands out four half angles rather than one field of view, since
// the two halves of an eye's frustum are not the same size on these headsets
void projectionFromFov(float* m, XrFovf fov, float nearZ, float farZ) {
    float left = tanf(fov.angleLeft);
    float right = tanf(fov.angleRight);
    float down = tanf(fov.angleDown);
    float up = tanf(fov.angleUp);
    float width = right - left;
    float height = up - down;

    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / width;
    m[5] = 2.0f / height;
    m[8] = (right + left) / width;
    m[9] = (up + down) / height;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

// The inverse of a pose that is only a rotation and a translation, which is
// what turns where an eye is into where the world is relative to it
void viewFromPose(float* m, XrPosef pose) {
    XrQuaternionf inv = quatConj(pose.orientation);
    Vec3 ex = { 1.0f, 0.0f, 0.0f };
    Vec3 ey = { 0.0f, 1.0f, 0.0f };
    Vec3 ez = { 0.0f, 0.0f, 1.0f };
    Vec3 rx = quatRotate(inv, ex);
    Vec3 ry = quatRotate(inv, ey);
    Vec3 rz = quatRotate(inv, ez);
    Vec3 eye = { pose.position.x, pose.position.y, pose.position.z };
    Vec3 t = quatRotate(inv, eye);

    m[0] = rx.x;  m[1] = rx.y;  m[2] = rx.z;  m[3] = 0.0f;
    m[4] = ry.x;  m[5] = ry.y;  m[6] = ry.z;  m[7] = 0.0f;
    m[8] = rz.x;  m[9] = rz.y;  m[10] = rz.z; m[11] = 0.0f;
    m[12] = -t.x; m[13] = -t.y; m[14] = -t.z; m[15] = 1.0f;
}

// Where the aim ray lands on the screen, in 0..1 texture coordinates with v
// running down the picture. Handles the cylinder as well, since the surface
// bulges toward the viewer and a flat approximation is wrong at the edges by
// the sagitta, which is a fifth of a metre on a wrapped 3 m screen.
int screenProject(XrPosef aim, XrPosef screen, float width, float height,
                         float radius, int curved, float* outU, float* outV) {
    XrQuaternionf inv = quatConj(screen.orientation);
    Vec3 aimPos = { aim.position.x, aim.position.y, aim.position.z };
    Vec3 screenPos = { screen.position.x, screen.position.y, screen.position.z };
    Vec3 forward = { 0.0f, 0.0f, -1.0f };

    // Both into the screen's own frame, where the surface sits in the xy plane
    Vec3 o = quatRotate(inv, vecSub(aimPos, screenPos));
    Vec3 d = quatRotate(inv, quatRotate(aim.orientation, forward));

    float hx, hy;
    if (curved) {
        // Axis is vertical through the cylinder centre, which sits behind the
        // surface by the radius. The viewer is inside, so there is one root.
        float cz = radius;
        float ox = o.x, oz = o.z - cz;
        float a = d.x * d.x + d.z * d.z;
        float b = 2.0f * (ox * d.x + oz * d.z);
        float c = ox * ox + oz * oz - radius * radius;
        if (a < 1e-6f) {
            return 0;
        }
        float disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) {
            return 0;
        }
        float t = (-b + sqrtf(disc)) / (2.0f * a);
        if (t <= 0.0f) {
            return 0;
        }
        float px = o.x + t * d.x;
        float py = o.y + t * d.y;
        float pz = o.z + t * d.z;
        // Angle off the centre of the arc, which faces -z from the axis
        float angle = atan2f(px, cz - pz);
        float centralAngle = width / radius;
        hx = angle / centralAngle;
        hy = py / height;
    }
    else {
        // The quad faces +z in its own frame, so the viewer has to be in front
        // of it and pointing back at it
        if (o.z <= 0.0f || d.z >= -1e-6f) {
            return 0;
        }
        float t = -o.z / d.z;
        hx = (o.x + t * d.x) / width;
        hy = (o.y + t * d.y) / height;
    }

    *outU = hx + 0.5f;
    // Texture rows run down the picture, world y runs up it
    *outV = 0.5f - hy;
    return 1;
}

// The inverse of screenHit: where a texture coordinate sits in space. The beam
// is drawn to the filtered point rather than the raw one, so the ray and the
// cursor agree instead of the ray shaking around a steady cursor.
Vec3 screenPoint(float u, float v, XrPosef screen, float width, float height,
                        float radius, int curved) {
    Vec3 local;
    local.y = (0.5f - v) * height;
    if (curved) {
        float angle = (u - 0.5f) * (width / radius);
        local.x = radius * sinf(angle);
        local.z = radius - radius * cosf(angle);
    }
    else {
        local.x = (u - 0.5f) * width;
        local.z = 0.0f;
    }

    Vec3 rotated = quatRotate(screen.orientation, local);
    Vec3 world = { screen.position.x + rotated.x,
                   screen.position.y + rotated.y,
                   screen.position.z + rotated.z };
    return world;
}

// Furniture pinned to the picture has to ride the picture. A curved screen
// bows toward the viewer at the edges, so anything left in the flat plane ends
// up behind the surface, and by more the wider the screen gets. Takes a point
// in the screen's flat local frame, where z is how far proud of the surface it
// should sit, and puts it on the cylinder. The yaw handed back turns a quad to
// lie along the surface there rather than cutting through it.
void curveLocal(Vec3* local, float radius, int curved, float* outYaw) {
    float yaw = 0.0f;
    if (curved && radius > 1e-3f) {
        // Local x is arc length from the centre, the same parameter screenPoint
        // and the cylinder layer use, so it holds outside the picture too
        float angle = local->x / radius;
        // Proud of the surface means toward the axis, which is past the viewer
        float proud = radius - local->z;
        local->x = proud * sinf(angle);
        local->z = radius - proud * cosf(angle);
        yaw = -angle;
    }
    if (outYaw != NULL) {
        *outYaw = yaw;
    }
}
