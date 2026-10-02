// Which display refresh rate to ask the runtime for, and whether the frame
// loop is holding the rate it is on. Plain arithmetic over the list of rates
// the runtime offers, with no OpenXR calls and no context, so the host tests
// reach all of it.

#ifndef XR_RATE_H
#define XR_RATE_H

#include <stdint.h>

// Runtimes report 90.0 and 119.88 alike, so rates this close are the same one
#define RATE_TOLERANCE 0.5f
// The frame budget never pushes the display below this. A stream asking for
// less than it still gets its own rate.
#define RATE_FLOOR_HZ 72.0f
// The most rates kept from the runtime's list
#define RATE_MAX 16
// The highest rate the debug property can force
#define RATE_KNOB_MAX 240

// The frame loop's own frame times are judged a window at a time. A rate is
// only stepped down after this many windows in a row over its period, so one
// slow moment, a scene cut or a room loading, never costs the session its rate.
#define RATE_WINDOW_NS 2000000000L
#define RATE_OVER_WINDOWS 3
// Windows thrown away after the rate, the 3D or the focus changes: the change
// itself costs a frame or two, and that is not what is being measured
#define RATE_SETTLE_WINDOWS 1
// Fewer warps than this in a window and its GPU time is not trusted. A desktop
// standing still sends next to no frames.
#define RATE_MIN_GPU_SAMPLES 30
// A frame loop that misses more than this share of the refreshes is not
// holding the rate either, whatever each warp measures on its own: a GPU kept
// busy end to end makes every frame late without any one of them being slow.
// Past it the loop's own pace, the time between the frames it submits, counts
// as its frame time. The odd miss at a scene cut stays well under it.
#define RATE_MISSED_PERCENT 10

// What one window said
#define RATE_WINDOW_FILLING  0
#define RATE_WINDOW_SETTLING 1
#define RATE_WINDOW_HELD     2
#define RATE_WINDOW_SLIPPING 3
#define RATE_WINDOW_OVER     4

// The offered rate matching hz, or 0 when the runtime has no such rate
float rateOffered(float hz, const float* rates, int count);

// The rate for a stream at fps: its own if offered; else, with multiples set,
// the lowest offered rate that is a whole multiple of it, which shows every
// frame for the same number of refreshes; else the nearest offered rate above
// it, else the highest offered. 0 with nothing offered or no fps.
float rateForStream(float fps, const float* rates, int count, int multiples);

// The next offered rate below hz that is still at least floorHz, or 0 when
// there is none
float rateStepDown(float hz, const float* rates, int count, float floorHz);

// The rate to ask for. The stream's rate with the 3D warp off, a whole
// multiple of it where the stream's own is not offered. With it on, never
// above heldHz, the rate the warp has been stepped down to (0 for none), and
// one offered step lower when frameMs, the frame time measured at that rate
// (0 for nothing measured), does not fit its period, though never below
// RATE_FLOOR_HZ. A held rate below the multiple rules the multiple out, so
// the stream goes to the nearest rate above it instead.
float rateChoose(float fps, const float* rates, int count, int warpOn, float heldHz,
                 float frameMs);

// Whether the session's display rate has settled, after which a change is
// news worth a toast: a throttle, a step down, a forced rate. It has once the
// session has drawn a focused frame and its first request has landed, was in
// force already, or was given up on; with nothing asked for, the focused frame
// is enough. Once settled it stays so, whatever is asked for later.
int rateSettled(int settled, int focusedFrame, float asked, int confirmed);

typedef struct {
    int64_t startNs;
    int settle;
    int overWindows;
    // The frame loop's own time from xrBeginFrame to xrEndFrame
    int64_t cpuTotalNs;
    long cpuFrames;
    // The warp's GPU time and the room's, per frame that drew them
    int64_t gpuTotalNs;
    long gpuFrames;
    int64_t roomTotalNs;
    long roomFrames;
    // Display refreshes the frame loop was not there for
    long missed;
    // What the last full window measured, for the log line. The frame time
    // is the slowest of the three that count.
    float cpuMs;
    float gpuMs;
    float paceMs;
    float frameMs;
    long lastMissed;
    long lastFrames;
    long lastGpuFrames;
} RateBudget;

// Starts measuring again from now, throwing the next settle windows away
void rateBudgetStart(RateBudget* b, int64_t nowNs, int settle);
void rateBudgetCpu(RateBudget* b, int64_t frameNs);
void rateBudgetGpu(RateBudget* b, int64_t gpuNs);
void rateBudgetRoom(RateBudget* b, int64_t gpuNs);
void rateBudgetMissed(RateBudget* b, long refreshes);

// Called once a frame. Says RATE_WINDOW_FILLING until a window is full, then
// what that window's frame time said against the period of hz: thrown away,
// held, slipping (over, but not yet for RATE_OVER_WINDOWS in a row), or over,
// which is the call to step down. The window's numbers stay in the budget for
// the log until the next one is full.
int rateBudgetTick(RateBudget* b, int64_t nowNs, float hz);

#endif
