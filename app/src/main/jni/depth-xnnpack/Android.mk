LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := depth-xnnpack
LOCAL_SRC_FILES := depth_xnnpack.c
# Only the JNI entry points are exported. LiteRT is looked up at run time
# rather than linked, since its library comes out of the AAR.
LOCAL_CFLAGS := -Wall -Werror -fvisibility=hidden
LOCAL_LDLIBS := -ldl -llog

include $(BUILD_SHARED_LIBRARY)
