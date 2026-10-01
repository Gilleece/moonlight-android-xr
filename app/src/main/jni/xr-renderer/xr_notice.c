// The fades, the splash's timing and the toast's queue. No GL and no
// context, so the host tests reach all of it.
#include "xr_notice.h"
#include "xr_shared.h"

int fadeStep(Fade* fade, int shown, int64_t now, int64_t durationNs, int smooth) {
    float target = shown ? 1.0f : 0.0f;
    int64_t dt = fade->lastNs != 0 ? now - fade->lastNs : 0;
    fade->lastNs = now;
    if (!smooth || durationNs <= 0) {
        fade->level = target;
        fade->running = 0;
        return FADE_NONE;
    }
    if (fade->level == target) {
        fade->running = 0;
        return FADE_NONE;
    }
    // A run starts whenever the level sets off, or turns round part way
    if (!fade->running || fade->rising != shown) {
        fade->running = 1;
        fade->rising = shown;
        fade->fromNs = now;
        fade->frames = 0;
    }
    fade->frames++;
    float step = dt > 0 ? (float)dt / (float)durationNs : 0.0f;
    fade->level += shown ? step : -step;
    if ((shown && fade->level >= 1.0f) || (!shown && fade->level <= 0.0f)) {
        fade->level = target;
        fade->running = 0;
        return shown ? FADE_IN_DONE : FADE_OUT_DONE;
    }
    return FADE_NONE;
}

int splashStep(Splash* splash, int64_t now, int waiting, int64_t fadeNs) {
    if (splash->firstNs == 0) {
        splash->firstNs = now;
    }
    int64_t up = now - splash->firstNs;
    if (splash->phase == SPLASH_UP) {
        if (up < SPLASH_FLOOR_NS || (waiting != 0 && up < SPLASH_CEILING_NS)) {
            return 0;
        }
        splash->liftNs = now;
        splash->waitingAtLift = waiting;
        splash->phase = fadeNs > 0 ? SPLASH_FADING : SPLASH_GONE;
        return 1;
    }
    if (splash->phase == SPLASH_FADING && now - splash->liftNs >= fadeNs) {
        splash->phase = SPLASH_GONE;
    }
    return 0;
}

float splashLevel(const Splash* splash, int64_t now, int64_t fadeNs) {
    if (splash->phase == SPLASH_UP) {
        return 1.0f;
    }
    if (splash->phase == SPLASH_GONE || fadeNs <= 0) {
        return 0.0f;
    }
    float level = 1.0f - (float)(now - splash->liftNs) / (float)fadeNs;
    return level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
}

int splashRow(const Splash* splash, int64_t now, int rows) {
    if (splash->firstNs == 0 || rows <= 0 || now < splash->firstNs) {
        return 0;
    }
    return (int)(((now - splash->firstNs) / SPLASH_DOT_NS) % rows);
}

void noticeInit(NoticeBoard* board) {
    board->count = 0;
    board->current.kind = -1;
    board->current.arg = 0;
    board->sinceNs = 0;
}

int noticeGroup(int kind) {
    switch (kind) {
        case TOAST_HANDS_LOCKED:
        case TOAST_HANDS_UNLOCKED:
            return TOAST_HANDS_LOCKED;
        case TOAST_3D_OFF:
        case TOAST_3D_ON:
            return TOAST_3D_OFF;
        default:
            return kind;
    }
}

void noticePush(NoticeBoard* board, int kind, int arg) {
    if (kind < 0) {
        return;
    }
    Notice notice = { kind, arg };
    for (int i = 0; i < board->count; i++) {
        if (noticeGroup(board->waiting[i].kind) == noticeGroup(kind)) {
            board->waiting[i] = notice;
            return;
        }
    }
    if (board->count == NOTICE_QUEUE) {
        for (int i = 1; i < NOTICE_QUEUE; i++) {
            board->waiting[i - 1] = board->waiting[i];
        }
        board->count--;
    }
    board->waiting[board->count++] = notice;
}

int noticeAdvance(NoticeBoard* board, int64_t now, int held, Notice* out) {
    if (board->current.kind >= 0 && now - board->sinceNs >= NOTICE_SHOW_NS) {
        board->current.kind = -1;
    }
    if (held || board->count == 0) {
        return 0;
    }
    Notice next = board->waiting[0];
    int turn = board->current.kind < 0
            || noticeGroup(next.kind) == noticeGroup(board->current.kind)
            || now - board->sinceNs >= NOTICE_MIN_NS;
    if (!turn) {
        return 0;
    }
    for (int i = 1; i < board->count; i++) {
        board->waiting[i - 1] = board->waiting[i];
    }
    board->count--;
    board->current = next;
    board->sinceNs = now;
    *out = next;
    return 1;
}

int noticeShowing(const NoticeBoard* board, int64_t now) {
    return board->current.kind >= 0 && now - board->sinceNs < NOTICE_SHOW_NS;
}
