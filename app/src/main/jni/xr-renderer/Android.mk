LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := xr-renderer
LOCAL_SRC_FILES := xr_renderer.c
LOCAL_CFLAGS := -Wall -Werror
LOCAL_LDLIBS := -lEGL -lGLESv3 -landroid -llog
LOCAL_SHARED_LIBRARIES := openxr_loader

include $(BUILD_SHARED_LIBRARY)

$(call import-module,prefab/OpenXR)
