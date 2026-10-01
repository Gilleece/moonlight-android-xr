LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := xr-renderer
LOCAL_SRC_FILES := xr_log.c \
                   xr_math.c \
                   xr_depthmap.c \
                   xr_atlas.c \
                   xr_roommesh.c \
                   xr_layout.c \
                   xr_rate.c \
                   xr_gate.c \
                   xr_pinch.c \
                   xr_notice.c \
                   xr_glow.c \
                   xr_grade.c \
                   xr_keys.c \
                   xr_controller.c \
                   xr_shaders.c \
                   xr_session.c \
                   xr_display.c \
                   xr_gl.c \
                   xr_depth.c \
                   xr_ambilight.c \
                   xr_room.c \
                   xr_model.c \
                   xr_input.c \
                   xr_ui.c \
                   xr_assets.c \
                   xr_debug.c \
                   xr_layers.c
# Only the JNI entry points are exported, the rest of the module stays private.
# The optimiser is asked for here rather than left to the build type: a debug
# build compiles native code at -O0, and the depth map's per texel loops run
# about three times slower there, which is what gets timed on a headset.
LOCAL_CFLAGS := -Wall -Werror -fvisibility=hidden -O2
# The setprop tuning knobs and frame capture, which the debug build type asks
# for and a release build leaves out
ifeq ($(XR_DEBUG_KNOBS),1)
LOCAL_CFLAGS += -DXR_DEBUG_KNOBS=1
endif
LOCAL_LDLIBS := -lEGL -lGLESv3 -landroid -llog
LOCAL_SHARED_LIBRARIES := openxr_loader

include $(BUILD_SHARED_LIBRARY)

$(call import-module,prefab/OpenXR)
