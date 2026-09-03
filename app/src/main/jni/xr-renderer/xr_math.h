
// Vectors, quaternions, the one euro filter and the small amount of
// projection maths the renderer needs. Nothing in here touches GL, OpenXR
// beyond its plain value types, or the context, so it can be built and
// exercised on a desktop as well as on the headset.

#ifndef XR_MATH_H
#define XR_MATH_H

#include <math.h>
#include <openxr/openxr.h>

// Derivative cutoff of the one euro filter, shared by both the scalar and
// the rotation versions
#define POINTER_D_CUTOFF 1.0f

typedef struct { float x, y, z; } Vec3;

// One euro filter: a low pass whose cutoff rises with speed, so a resting
// hand is smoothed hard while a fast sweep is barely delayed
typedef struct {
    int valid;
    float x;
    float dx;
} EuroState;

// One euro on a rotation: angular speed drives the cutoff and the blend
// is a lerp toward the new sample, which is fine at per frame angles
typedef struct {
    int valid;
    XrQuaternionf q;
    float dAngle;
} EuroQuatState;

Vec3 vecSub(Vec3 a, Vec3 b);
Vec3 vecNorm(Vec3 v);
Vec3 vecCross(Vec3 a, Vec3 b);
float vecDot(Vec3 a, Vec3 b);

XrQuaternionf quatConj(XrQuaternionf q);
XrQuaternionf quatMul(XrQuaternionf a, XrQuaternionf b);
XrQuaternionf quatNorm(XrQuaternionf q);
XrQuaternionf axisAngleQuat(Vec3 axis, float angle);
Vec3 quatRotate(XrQuaternionf q, Vec3 v);
XrQuaternionf quatFromBasis(Vec3 x, Vec3 y, Vec3 z);

float euroFilter(EuroState* s, float x, float dt, float minCutoff, float beta);
XrQuaternionf euroFilterQuat(EuroQuatState* s, XrQuaternionf q, float dt,
                             float minCutoff, float beta);

void matMul(float* out, const float* a, const float* b);
void projectionFromFov(float* m, XrFovf fov, float nearZ, float farZ);
void viewFromPose(float* m, XrPosef pose);

// Where a ray lands on the screen and where a point on the screen sits in
// space, on the flat quad and on the cylinder alike
int screenProject(XrPosef aim, XrPosef screen, float width, float height,
                  float radius, int curved, float* outU, float* outV);
Vec3 screenPoint(float u, float v, XrPosef screen, float width, float height,
                 float radius, int curved);
void curveLocal(Vec3* local, float radius, int curved, float* outYaw);

#endif
