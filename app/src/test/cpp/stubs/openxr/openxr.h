// Just the value types the maths modules use, laid out the way the OpenXR
// headers lay them out, so those modules can be built and tested on a desktop
// without the loader's SDK
#ifndef OPENXR_H_
#define OPENXR_H_

typedef struct XrVector3f {
    float x;
    float y;
    float z;
} XrVector3f;

typedef struct XrQuaternionf {
    float x;
    float y;
    float z;
    float w;
} XrQuaternionf;

typedef struct XrPosef {
    XrQuaternionf orientation;
    XrVector3f position;
} XrPosef;

typedef struct XrFovf {
    float angleLeft;
    float angleRight;
    float angleUp;
    float angleDown;
} XrFovf;

#endif
