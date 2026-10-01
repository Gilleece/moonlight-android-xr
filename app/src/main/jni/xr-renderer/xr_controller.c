// The controllers as they are drawn, see xr_controller.h
#include "xr_controller.h"

int raySwitchOn(int settingOn, int flipped) {
    return (settingOn != 0) != (flipped != 0);
}

int rayFlipFor(int settingOn, int wantOn) {
    return (settingOn != 0) != (wantOn != 0);
}

int rayDrawn(int settingOn, int flipped, int panelOpen) {
    return panelOpen != 0 || raySwitchOn(settingOn, flipped);
}
