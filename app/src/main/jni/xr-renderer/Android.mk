LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := xr-renderer
LOCAL_SRC_FILES := xr_log.c \
                   xr_math.c \
                   xr_depthmap.c \
                   xr_shaders.c \
                   xr_session.c \
                   xr_gl.c \
                   xr_depth.c \
                   xr_ambilight.c \
                   xr_room.c \
                   xr_input.c \
                   xr_ui.c \
                   xr_assets.c \
                   xr_debug.c \
                   xr_layers.c
# Only the JNI entry points are exported, the rest of the module stays private
LOCAL_CFLAGS := -Wall -Werror -fvisibility=hidden
LOCAL_LDLIBS := -lEGL -lGLESv3 -landroid -llog
LOCAL_SHARED_LIBRARIES := openxr_loader

include $(BUILD_SHARED_LIBRARY)

$(call import-module,prefab/OpenXR)
