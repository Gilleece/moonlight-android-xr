// The keyboard's sheets and its held modifiers. See xr_keys.h.
#include "xr_keys.h"

int kbModifierBit(int code) {
    switch (code) {
        case KB_CODE_CTRL:
            return KB_MOD_CTRL;
        case KB_CODE_ALT:
            return KB_MOD_ALT;
        case KB_CODE_WIN:
            return KB_MOD_WIN;
        default:
            return 0;
    }
}

KbPress kbPress(int sheet, int mods, int code) {
    KbPress p;
    p.sheet = sheet;
    p.mods = mods;
    p.code = -1;
    p.codeMods = 0;
    p.hide = 0;

    int bit = kbModifierBit(code);
    if (bit != 0) {
        p.mods = mods ^ bit;
    }
    else if (code == KB_CODE_SHIFT) {
        // Shift off the symbols page goes to the capitals rather than back
        // where it came from
        p.sheet = sheet == KB_STATE_UPPER ? KB_STATE_LOWER : KB_STATE_UPPER;
    }
    else if (code == KB_CODE_SYMBOLS) {
        // The ABC key on the symbols and the Fn sheet, ?123 on the letters
        p.sheet = sheet == KB_STATE_SYMBOLS || sheet == KB_STATE_FN ? KB_STATE_LOWER
                                                                    : KB_STATE_SYMBOLS;
    }
    else if (code == KB_CODE_FN) {
        // Fn on the symbols, and ?123 in the same place on the Fn sheet
        p.sheet = sheet == KB_STATE_FN ? KB_STATE_SYMBOLS : KB_STATE_FN;
    }
    else if (code == KB_CODE_HIDE) {
        p.hide = 1;
        p.mods = 0;
    }
    else if (code > 0) {
        p.code = code;
        p.codeMods = mods;
        p.mods = 0;
        // Shift is one shot over the letters, the way a phone keyboard
        // behaves, and sticky over the punctuation row above them
        if (sheet == KB_STATE_UPPER && code >= 'A' && code <= 'Z') {
            p.sheet = KB_STATE_LOWER;
        }
    }
    return p;
}
