// Every controller at once: the GamePad, Wii Remotes (with or without a Nunchuk), Classic and Pro Controllers.
#pragma once
#include <stdint.h>
#include <padscore/kpad.h>
#include <vpad/input.h>

typedef struct {
    uint32_t hold;      // VPAD_BUTTON_* for menus (a Wii Remote's B is B, its Nunchuk's Z and C are L and R)
    uint32_t game_hold; // the same for games, without the buttons that click instead
    int click_l, click_r;
    float lx, ly, rx, ry; // sticks, -1..1, y up
    int pointer_valid;    // a Wii Remote aimed at the sensor bar; (px, py) is where, in 1280x720
    float px, py;
} Pads;

void pads_init(void);
// vs / err: this frame's VPADRead. Without a new sample a controller keeps its last state.
void pads_read(const VPADStatus* vs, VPADReadError err, Pads* out);
// The last sample from a Wii Remote channel (0..3), for the system keyboard; NULL when nothing is connected.
const KPADStatus* pads_kpad(int chan);
