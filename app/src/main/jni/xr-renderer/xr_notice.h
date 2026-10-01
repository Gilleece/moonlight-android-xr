// The timing behind what comes and goes in front of the session: the panels
// fading in and out, and the splash that covers the first seconds while the
// panels, the room and the depth model come up. Plain arithmetic on a clock
// passed in, with no GL, OpenXR or context, so the host tests reach all of it.

#ifndef XR_NOTICE_H
#define XR_NOTICE_H

#include <stdint.h>

// How long a panel takes to fade all the way in or out, and the splash
#define FADE_NS 150000000LL
#define SPLASH_FADE_NS 300000000LL

// What fadeStep says on the frame a fade lands
#define FADE_NONE 0
#define FADE_IN_DONE 1
#define FADE_OUT_DONE 2

// A layer's opacity on its way to shown or hidden, with the run it is on so
// the caller can say how long it took
typedef struct {
    float level;
    int64_t lastNs;
    int64_t fromNs;
    int frames;
    int rising;
    int running;
} Fade;

// Moves a fade toward 1 while shown and toward 0 while not, a full swing per
// durationNs. Without smooth it jumps there, which is a runtime that cannot
// fade a layer. Returns FADE_IN_DONE or FADE_OUT_DONE on the frame a fade that
// was under way lands, otherwise FADE_NONE.
int fadeStep(Fade* fade, int shown, int64_t now, int64_t durationNs, int smooth);

// The splash is held for at least the floor, so a warm start does not flash
// it past, and at most the ceiling, so a model that never answers does not
// hide the stream. The word under the name gains a dot every DOT_NS.
#define SPLASH_FLOOR_NS 1500000000LL
#define SPLASH_CEILING_NS 8000000000LL
#define SPLASH_DOT_NS 300000000LL

// What the splash can still be waiting on, as bits
#define SPLASH_WAIT_PANELS 1
#define SPLASH_WAIT_ROOM 2
#define SPLASH_WAIT_DEPTH 4

#define SPLASH_UP 0
#define SPLASH_FADING 1
#define SPLASH_GONE 2

typedef struct {
    // The session's first frame, 0 until there has been one
    int64_t firstNs;
    // When it began to go, and what it was still waiting on then
    int64_t liftNs;
    int waitingAtLift;
    int phase;
} Splash;

// One frame of the splash, given what is still not ready. Returns 1 on the
// frame it lifts, so the caller can say why once. A fade of 0 cuts it.
int splashStep(Splash* splash, int64_t now, int waiting, int64_t fadeNs);

// How opaque it is now: 1 while up, falling to 0 over the fade
float splashLevel(const Splash* splash, int64_t now, int64_t fadeNs);

// Which of the dot rows is showing: one dot, two, then three, and round again
int splashRow(const Splash* splash, int64_t now, int rows);

// The toast says one thing at a time for NOTICE_SHOW_NS. A notice about the
// same thing as the one up replaces it at once, being the newer word on it,
// and one about something else waits until the one up has had NOTICE_MIN_NS.
// A few can wait their turn; past that the oldest waiting is dropped.
#define NOTICE_SHOW_NS 4000000000LL
#define NOTICE_MIN_NS 1500000000LL
#define NOTICE_QUEUE 4

typedef struct {
    int kind;
    int arg;
} Notice;

typedef struct {
    Notice waiting[NOTICE_QUEUE];
    int count;
    // What is up and since when, kind -1 while nothing is
    Notice current;
    int64_t sinceNs;
} NoticeBoard;

void noticeInit(NoticeBoard* board);

// Which notices are about the same thing: a lock and an unlock, the 3D going
// off and on, head aim going off and on, two display rates, two messages
int noticeGroup(int kind);

// Queued to be said. One about the same thing already waiting is replaced
// where it stands.
void noticePush(NoticeBoard* board, int kind, int arg);

// Retires the notice up once its time is over and puts the next one up when
// its turn has come, unless held, which is how the splash keeps them back.
// Returns 1 on the frame a notice goes up, with it in out.
int noticeAdvance(NoticeBoard* board, int64_t now, int held, Notice* out);

// Whether a notice is up and still within its time
int noticeShowing(const NoticeBoard* board, int64_t now);

#endif
