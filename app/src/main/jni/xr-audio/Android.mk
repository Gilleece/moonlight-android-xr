LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := xr-audio
LOCAL_SRC_FILES := xr_audio.c
# Only the JNI entry points are exported. The convolution is the whole cost
# of the virtual surround, so it is optimised in debug builds too, which
# would otherwise compile it at -O0.
LOCAL_CFLAGS := -Wall -Werror -fvisibility=hidden -O2

include $(BUILD_SHARED_LIBRARY)
