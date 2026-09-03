// The native half of the file log. Lines go to logcat as they always did,
// and to the file the Java side opened when the user turned it on.
#include "xr_renderer.h"

// Where the Java side put the log, if the user turned it on. Globals rather
// than context fields, since this is configured before any context exists.
static char fileLogPath[512];
static int fileLogLevel = FILE_LOG_OFF;

// The log is held open rather than reopened per line. In the Download folder
// every open goes through the media provider, which measured 339 us on
// average and 10 ms at worst, and this writes from the render and depth
// threads with nothing between it and the frame.
static int logFd = -1;
static ino_t logIno;
static pthread_mutex_t logMutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Appends one line to the file the Java logger is also writing, in a single
 * write so the two writers cannot interleave halfway through a line.
 *
 * The fd is kept across calls, so it has to be checked before each append:
 * when the log fills, the Java side deletes it out from under us, and the fd
 * would go on filling an inode nothing can reach any more. Comparing the
 * inode at the path against the one we opened catches that, and a user
 * deleting the file by hand, for one stat rather than one open.
 */
static void writeFileLog(int prio, const char* fmt, va_list args) {
    char level;
    if (prio == ANDROID_LOG_ERROR) {
        level = 'E';
    }
    else if (prio == ANDROID_LOG_WARN) {
        level = 'W';
    }
    else if (prio == PRIO_EVENT) {
        level = 'I';
    }
    else if (fileLogLevel >= FILE_LOG_VERBOSE) {
        level = 'V';
    }
    else {
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm parts;
    localtime_r(&ts.tv_sec, &parts);

    char line[1024];
    int n = snprintf(line, sizeof(line), "%02d-%02d %02d:%02d:%02d.%03d %c xr: ",
                     parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min,
                     parts.tm_sec, (int)(ts.tv_nsec / 1000000), level);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        return;
    }

    size_t room = sizeof(line) - (size_t)n - 1;
    int m = vsnprintf(line + n, room, fmt, args);
    if (m < 0) {
        return;
    }
    if ((size_t)m >= room) {
        m = (int)room - 1;
    }
    n += m;
    line[n++] = '\n';

    pthread_mutex_lock(&logMutex);

    if (logFd >= 0) {
        struct stat current;
        if (stat(fileLogPath, &current) != 0 || current.st_ino != logIno) {
            close(logFd);
            logFd = -1;
        }
    }

    if (logFd < 0) {
        struct stat opened;
        logFd = open(fileLogPath, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (logFd < 0 || fstat(logFd, &opened) != 0) {
            if (logFd >= 0) {
                close(logFd);
                logFd = -1;
            }
            // The next line tries again
            pthread_mutex_unlock(&logMutex);
            return;
        }
        logIno = opened.st_ino;
    }

    ssize_t ignored = write(logFd, line, (size_t)n);
    (void)ignored;

    pthread_mutex_unlock(&logMutex);
}

void xrLog(int prio, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(prio == PRIO_EVENT ? ANDROID_LOG_INFO : prio, TAG, fmt, args);
    va_end(args);

    if (fileLogLevel == FILE_LOG_OFF || fileLogPath[0] == '\0') {
        return;
    }
    va_start(args, fmt);
    writeFileLog(prio, fmt, args);
    va_end(args);
}

// Points the native log lines at the file the Java side opened. Called before
// init, so there is no context to hang it off yet.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetFileLog(JNIEnv* env, jclass clazz,
                                                             jstring path, jint level) {
    const char* chars = NULL;
    if (path != NULL && level > FILE_LOG_OFF) {
        chars = (*env)->GetStringUTFChars(env, path, NULL);
    }

    pthread_mutex_lock(&logMutex);
    // Whatever is cached was opened against the old path, so it goes first
    if (logFd >= 0) {
        close(logFd);
        logFd = -1;
    }
    logIno = 0;

    if (chars == NULL) {
        fileLogLevel = FILE_LOG_OFF;
        fileLogPath[0] = '\0';
    }
    else {
        strncpy(fileLogPath, chars, sizeof(fileLogPath) - 1);
        fileLogPath[sizeof(fileLogPath) - 1] = '\0';
        fileLogLevel = level;
    }
    pthread_mutex_unlock(&logMutex);

    if (chars != NULL) {
        (*env)->ReleaseStringUTFChars(env, path, chars);
    }
}
