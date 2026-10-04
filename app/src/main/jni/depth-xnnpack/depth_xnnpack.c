// LiteRT's XNNPACK delegate on a chosen number of threads, for the depth
// model's CPU route. The default XNNPACK path runs one thread whatever the
// interpreter is asked for, and LiteRT has no Java class for the delegate,
// but the runtime library its AAR ships exports the C functions. They are
// looked up there once Java has loaded it. Anything missing means no
// delegate, and the model stays on the default path.
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>

#include <android/log.h>

#define TAG "DepthXnnpack"

// The options struct is not in the headers the AAR ships. All this relies on
// is that the thread count comes first and that the struct fits here: in
// LiteRT 1.4.2 it is 64 bytes on arm64, less on the 32 bit ABIs. The count
// is read back off the delegate, so a layout that has moved turns into no
// delegate rather than a wrong one.
typedef struct {
    int32_t numThreads;
    unsigned char rest[508];
} __attribute__((aligned(16))) XnnpackOptions;

typedef XnnpackOptions (*OptionsDefaultFn)(void);
typedef void* (*CreateFn)(const XnnpackOptions* options);
typedef void (*DeleteFn)(void* delegate);
typedef const int32_t* (*GetOptionsFn)(void* delegate);
typedef void* (*GetThreadPoolFn)(void* delegate);

static OptionsDefaultFn optionsDefault;
static CreateFn create;
static DeleteFn destroy;
static GetOptionsFn getOptions;
static GetThreadPoolFn getThreadPool;
static pthread_once_t lookUpOnce = PTHREAD_ONCE_INIT;

static void* find(void* lib, const char* name) {
    void* fn = dlsym(lib, name);
    if (fn == NULL) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "LiteRT has no %s", name);
    }
    return fn;
}

static void lookUp(void) {
    // Java loaded it already, so this finds that copy and never loads another.
    // The handle is kept: the runtime stays loaded for the process anyway.
    void* lib = dlopen("libtensorflowlite_jni.so", RTLD_NOW | RTLD_NOLOAD);
    if (lib == NULL) {
        const char* why = dlerror();
        __android_log_print(ANDROID_LOG_WARN, TAG, "LiteRT is not loaded: %s",
                            why != NULL ? why : "no reason given");
        return;
    }
    OptionsDefaultFn foundDefault =
            (OptionsDefaultFn) find(lib, "TfLiteXNNPackDelegateOptionsDefault");
    CreateFn foundCreate = (CreateFn) find(lib, "TfLiteXNNPackDelegateCreate");
    DeleteFn foundDelete = (DeleteFn) find(lib, "TfLiteXNNPackDelegateDelete");
    GetOptionsFn foundOptions = (GetOptionsFn) find(lib, "TfLiteXNNPackDelegateGetOptions");
    GetThreadPoolFn foundPool = (GetThreadPoolFn) find(lib, "TfLiteXNNPackDelegateGetThreadPool");
    if (foundDefault == NULL || foundCreate == NULL || foundDelete == NULL
            || foundOptions == NULL || foundPool == NULL) {
        return;
    }
    optionsDefault = foundDefault;
    destroy = foundDelete;
    getOptions = foundOptions;
    getThreadPool = foundPool;
    create = foundCreate;
}

JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XnnpackDelegate_nativeCreate(
        JNIEnv* env, jclass clazz, jint threads) {
    (void) env;
    (void) clazz;
    pthread_once(&lookUpOnce, lookUp);
    if (create == NULL) {
        return 0;
    }
    XnnpackOptions options = optionsDefault();
    options.numThreads = threads;
    void* delegate = create(&options);
    if (delegate == NULL) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "XNNPACK delegate on %d threads not created",
                            threads);
        return 0;
    }
    // The delegate keeps its own copy of the options and makes its thread
    // pool from it, so check both: one thread runs without a pool
    const int32_t* held = getOptions(delegate);
    int heldThreads = held != NULL ? *held : -1;
    int pooled = getThreadPool(delegate) != NULL;
    if (heldThreads != threads || pooled != (threads > 1)) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
                            "XNNPACK delegate asked for %d threads came back with %d and %s pool",
                            threads, heldThreads, pooled ? "a" : "no");
        destroy(delegate);
        return 0;
    }
    return (jlong) (intptr_t) delegate;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XnnpackDelegate_nativeDelete(
        JNIEnv* env, jclass clazz, jlong handle) {
    (void) env;
    (void) clazz;
    // A handle only exists once the lookup has found everything
    if (handle != 0 && destroy != NULL) {
        destroy((void*) (intptr_t) handle);
    }
}
