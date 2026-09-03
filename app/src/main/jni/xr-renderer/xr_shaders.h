
// The GLSL the renderer compiles at startup. Kept as C strings so they ship
// inside the library and need no asset loading.

#ifndef XR_SHADERS_H
#define XR_SHADERS_H

extern const char* const VERTEX_SRC;
extern const char* const FRAGMENT_SRC;
extern const char* const UPSAMPLE_FRAGMENT_SRC;
extern const char* const OFFSET_FRAGMENT_SRC;
extern const char* const DOWNSCALE_FRAGMENT_SRC;
extern const char* const AMBI_FRAGMENT_SRC;
extern const char* const GLOW_FRAGMENT_SRC;
extern const char* const ROOM_VERTEX_SRC;
extern const char* const ROOM_FRAGMENT_SRC;

#endif
