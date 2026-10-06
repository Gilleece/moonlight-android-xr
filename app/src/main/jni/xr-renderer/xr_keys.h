// The in world keyboard's own rules: which sheet a press leaves it on, which
// of Ctrl, Alt and Win are held, and what goes to the host with them. Plain
// arithmetic over the codes Java's layout hands down, no context, so the host
// tests reach all of it.

#ifndef XR_KEYS_H
#define XR_KEYS_H

#include "xr_shared.h"

// What one press does
typedef struct {
    // The sheet showing after it, a KB_STATE_
    int sheet;
    // The modifiers lit after it, KB_MOD_ bits
    int mods;
    // What to type, or -1 for nothing, and the modifiers held while it goes
    int code;
    int codeMods;
    // 1 if it put the keyboard away
    int hide;
} KbPress;

// A press on a key with this code, on this sheet, with these modifiers lit.
// A modifier key lights or puts out its own bit. Any other key that types
// goes with whatever is lit and puts them all out. The sheet keys leave the
// modifiers alone, so a Ctrl lit on the Fn sheet still holds over the
// letters, and hiding the keyboard lets go of them.
KbPress kbPress(int sheet, int mods, int code);

// The modifier a key code lights, a KB_MOD_ bit, or 0
int kbModifierBit(int code);

#endif
