// Which display refresh rate to ask the runtime for, whether the frame loop
// is holding the rate it is on, and how often the depth model runs, which is
// cut before the display rate is. Plain arithmetic over the list of rates the
// runtime offers and the frame loop's clock, with no OpenXR calls and no
// context, so the host tests reach all of it.

#ifndef XR_RATE_H
#define XR_RATE_H

#include <stdint.h>

#include "xr_shared.h"

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

// The depth model's rate, in maps a second, DEPTH_RATE_MIN to DEPTH_RATE_MAX.
// The preference sets the most it may run at, and the frame loop captures by
// time rather than by frame count, so a 120 fps stream costs the model what a
// 60 fps one does.

// After a cut the budget is given this long to show it before the next move
#define DEPTH_HOLD_NS 10000000000LL
// The budget has to hold this long without a break before the rate climbs a
// step. Each step up that the budget then fails doubles it, up to
// DEPTH_RECOVER_MAX_NS, and a step up that holds DEPTH_RAISE_HELD_NS puts it
// back.
#define DEPTH_RECOVER_NS 30000000000LL
#define DEPTH_RECOVER_MAX_NS 300000000000LL
#define DEPTH_RAISE_HELD_NS 120000000000LL

// The preference's value in range
int depthRateClamp(int perSecond);

// When the next capture is due, and when the gate last saw a new frame
typedef struct {
    int64_t dueNs;
    int64_t lastNs;
} DepthGate;

// Whether the new frame in hand at nowNs is the one to capture for a rate of
// perSecond. Captures come a period apart counted from when each was due, not
// from when it was taken, so the rate holds on average whatever the frame
// rate, and the frame nearest the due time is the one taken. A gate more than
// a period behind starts again from now rather than catching up in a burst.
int depthGateDue(DepthGate* g, int64_t nowNs, int perSecond);

// A capture taken at nowNs regardless of the gate, as when the 3D comes back
// on, which the next one is then counted from
void depthGateTaken(DepthGate* g, int64_t nowNs, int perSecond);

// What the governor did with a judged window or a notice from the runtime
#define DEPTH_MOVE_NONE    0
// The depth target halved
#define DEPTH_MOVE_CUT     1
// Doubled again, up to the preference
#define DEPTH_MOVE_RAISED  2
// Over budget, but within DEPTH_HOLD_NS of a cut that has yet to show
#define DEPTH_MOVE_HOLDING 3
// Over budget with the depth target already at DEPTH_RATE_MIN, or no model
// running to cut: the display rate's turn to step down
#define DEPTH_MOVE_DISPLAY 4
// The last step up held DEPTH_RAISE_HELD_NS, so the wait went back to
// DEPTH_RECOVER_NS
#define DEPTH_MOVE_RAISE_HELD 5
// The budget held long enough to climb, but the session's failed target
// keeps it where it is. Said once per failure.
#define DEPTH_MOVE_CAPPED 6

// The depth rate spent before the display rate. Over budget, the target is
// halved first, down to DEPTH_RATE_MIN, each cut held DEPTH_HOLD_NS before
// another; only past that does the display step down. After the recovery wait
// of held budget it doubles again, a step at a time, up to the preference,
// but never past half of the last target the budget failed at this session,
// so a rate the headset cannot hold is not tried again and again.
typedef struct {
    // The preference and what the gate runs at now, maps a second
    int cap;
    int target;
    // When the target was last cut, 0 for never
    int64_t cutNs;
    // Since when every judged window has held, 0 for not holding
    int64_t heldSinceNs;
    // The runtime says a CPU or GPU domain is throttled, which keeps the
    // target from climbing
    int throttled;
    // The last target the budget failed at, 0 for none. A throttle is the
    // runtime's doing and does not count.
    int failedTarget;
    // The held budget the next step up waits for, and when the last step up
    // was, 0 once it has been judged
    int64_t recoverNs;
    int64_t raisedNs;
    // The failed target's ceiling has been said since the last cut
    int cappedSaid;
} DepthGovernor;

void depthGovernorStart(DepthGovernor* g, int cap);

// A new preference, mid session or at its start. A different one starts the
// governor again at it, the failed target and the recovery wait forgotten,
// and returns 1; the same one changes nothing and returns 0.
int depthGovernorSetCap(DepthGovernor* g, int cap);

// The most the target climbs back to: the preference, or half the failed
// target if there is one, though never under DEPTH_RATE_MIN
int depthGovernorCeiling(const DepthGovernor* g);

// The recovery wait after a step up fails: doubled, up to DEPTH_RECOVER_MAX_NS
int64_t depthRecoverBackoff(int64_t waitNs);

// A judged window from rateBudgetTick at nowNs. live says the depth model is
// running, with the 3D on: without it there is nothing to cut or raise.
int depthGovernorWindow(DepthGovernor* g, int64_t nowNs, int verdict, int live);

// A performance notice from the runtime: worse when a domain moved to a
// warning or worse, throttled while any domain is still off normal. A throttle
// cuts like a budget miss, held the same way; it never steps the display.
int depthGovernorThrottle(DepthGovernor* g, int64_t nowNs, int worse, int throttled, int live);

#endif
