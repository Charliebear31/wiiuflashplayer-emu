#include "pads.h"
#include <math.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <string.h>

#define NCHAN 4

typedef struct { uint32_t from, to; } Map;

static const Map CORE_BOTH[] = {
    { WPAD_BUTTON_LEFT, VPAD_BUTTON_LEFT }, { WPAD_BUTTON_RIGHT, VPAD_BUTTON_RIGHT },
    { WPAD_BUTTON_UP, VPAD_BUTTON_UP }, { WPAD_BUTTON_DOWN, VPAD_BUTTON_DOWN },
    { WPAD_BUTTON_A, VPAD_BUTTON_A }, { WPAD_BUTTON_1, VPAD_BUTTON_X }, { WPAD_BUTTON_2, VPAD_BUTTON_Y },
    { WPAD_BUTTON_PLUS, VPAD_BUTTON_PLUS }, { WPAD_BUTTON_MINUS, VPAD_BUTTON_MINUS },
};

// The Classic and Pro Controllers' buttons have the same values for these.
static const Map PRO[] = {
    { WPAD_PRO_BUTTON_LEFT, VPAD_BUTTON_LEFT }, { WPAD_PRO_BUTTON_RIGHT, VPAD_BUTTON_RIGHT },
    { WPAD_PRO_BUTTON_UP, VPAD_BUTTON_UP }, { WPAD_PRO_BUTTON_DOWN, VPAD_BUTTON_DOWN },
    { WPAD_PRO_BUTTON_A, VPAD_BUTTON_A }, { WPAD_PRO_BUTTON_B, VPAD_BUTTON_B },
    { WPAD_PRO_BUTTON_X, VPAD_BUTTON_X }, { WPAD_PRO_BUTTON_Y, VPAD_BUTTON_Y },
    { WPAD_PRO_BUTTON_PLUS, VPAD_BUTTON_PLUS }, { WPAD_PRO_BUTTON_MINUS, VPAD_BUTTON_MINUS },
    { WPAD_PRO_BUTTON_L, VPAD_BUTTON_L }, { WPAD_PRO_BUTTON_R, VPAD_BUTTON_R },
    { WPAD_PRO_BUTTON_ZL, VPAD_BUTTON_ZL }, { WPAD_PRO_BUTTON_ZR, VPAD_BUTTON_ZR },
};

static uint32_t mapped(uint32_t hold, const Map* m, int n) {
    uint32_t out = 0;
    for (int i = 0; i < n; i++)
        if (hold & m[i].from) out |= m[i].to;
    return out;
}

// The strongest push wins, whichever controller it came from.
static void stick_in(float* have, float v) {
    if (fabsf(v) > fabsf(*have)) *have = v;
}

static Pads g_chan[NCHAN];
static Pads g_vpad;
static KPADStatus g_kpad[NCHAN];
static int g_kpad_valid[NCHAN];

const KPADStatus* pads_kpad(int chan) {
    return chan >= 0 && chan < NCHAN && g_kpad_valid[chan] ? &g_kpad[chan] : NULL;
}

static void read_chan(int c) {
    KPADStatus st;
    KPADError err = KPAD_ERROR_OK;
    memset(&st, 0, sizeof(st));
    int32_t got = KPADReadEx((KPADChan)c, &st, 1, &err);
    Pads* p = &g_chan[c];
    if (err == KPAD_ERROR_INVALID_CONTROLLER) { // not connected
        memset(p, 0, sizeof(*p));
        g_kpad_valid[c] = 0;
        return;
    }
    if (got <= 0 || st.error != KPAD_ERROR_OK) return; // no new sample: as it was
    g_kpad[c] = st;
    g_kpad_valid[c] = 1;
    memset(p, 0, sizeof(*p));
    int n_core = (int)(sizeof(CORE_BOTH) / sizeof(CORE_BOTH[0])), n_pro = (int)(sizeof(PRO) / sizeof(PRO[0]));
    switch (st.extensionType) {
    case WPAD_EXT_PRO_CONTROLLER:
        p->hold = p->game_hold = mapped(st.pro.hold, PRO, n_pro);
        p->lx = st.pro.leftStick.x; p->ly = st.pro.leftStick.y;
        p->rx = st.pro.rightStick.x; p->ry = st.pro.rightStick.y;
        p->click_l = (st.pro.hold & WPAD_PRO_BUTTON_L) != 0;
        p->click_r = (st.pro.hold & WPAD_PRO_BUTTON_R) != 0;
        return;
    case WPAD_EXT_CLASSIC:
    case WPAD_EXT_MPLUS_CLASSIC:
        p->hold = p->game_hold = mapped(st.classic.hold, PRO, n_pro);
        p->lx = st.classic.leftStick.x; p->ly = st.classic.leftStick.y;
        p->rx = st.classic.rightStick.x; p->ry = st.classic.rightStick.y;
        p->click_l = (st.classic.hold & WPAD_CLASSIC_BUTTON_L) != 0;
        p->click_r = (st.classic.hold & WPAD_CLASSIC_BUTTON_R) != 0;
        return;
    default:
        break;
    }
    // A Wii Remote: aim to move the mouse, B clicks. With a Nunchuk its stick moves the mouse too; Z and C click.
    p->hold = p->game_hold = mapped(st.hold, CORE_BOTH, n_core);
    if (st.hold & WPAD_BUTTON_B) {
        p->hold |= VPAD_BUTTON_B;
        p->click_l = 1;
    }
    if (st.extensionType == WPAD_EXT_NUNCHUK || st.extensionType == WPAD_EXT_MPLUS_NUNCHUK) {
        p->lx = st.nunchuk.stick.x;
        p->ly = st.nunchuk.stick.y;
        if (st.nunchuk.hold & WPAD_NUNCHUK_BUTTON_Z) {
            p->hold |= VPAD_BUTTON_L;
            p->click_l = 1;
        }
        if (st.nunchuk.hold & WPAD_NUNCHUK_BUTTON_C) {
            p->hold |= VPAD_BUTTON_R;
            p->click_r = 1;
        }
    }
    if (st.posValid > 0) {
        p->pointer_valid = 1;
        p->px = (st.pos.x + 1.0f) * 640.0f;
        p->py = (st.pos.y + 1.0f) * 360.0f;
    }
}

void pads_init(void) {
    KPADInit();
    WPADEnableURCC(TRUE);
    WPADEnableWiiRemote(TRUE);
}

void pads_read(const VPADStatus* vs, VPADReadError err, Pads* out) {
    if (err == VPAD_READ_SUCCESS) {
        memset(&g_vpad, 0, sizeof(g_vpad));
        g_vpad.hold = g_vpad.game_hold = vs->hold;
        g_vpad.lx = vs->leftStick.x; g_vpad.ly = vs->leftStick.y;
        g_vpad.rx = vs->rightStick.x; g_vpad.ry = vs->rightStick.y;
        g_vpad.click_l = (vs->hold & VPAD_BUTTON_L) != 0;
        g_vpad.click_r = (vs->hold & VPAD_BUTTON_R) != 0;
    }
    *out = g_vpad;
    for (int c = 0; c < NCHAN; c++) {
        read_chan(c);
        const Pads* p = &g_chan[c];
        out->hold |= p->hold;
        out->game_hold |= p->game_hold;
        out->click_l |= p->click_l;
        out->click_r |= p->click_r;
        stick_in(&out->lx, p->lx);
        stick_in(&out->ly, p->ly);
        stick_in(&out->rx, p->rx);
        stick_in(&out->ry, p->ry);
        if (p->pointer_valid && !out->pointer_valid) {
            out->pointer_valid = 1;
            out->px = p->px;
            out->py = p->py;
        }
    }
    // Sticks on the other controllers act as directions in menus, as the GamePad's do by themselves.
    uint32_t dirs = 0;
    if (out->lx < -0.5f) dirs |= VPAD_STICK_L_EMULATION_LEFT;
    if (out->lx > 0.5f) dirs |= VPAD_STICK_L_EMULATION_RIGHT;
    if (out->ly > 0.5f) dirs |= VPAD_STICK_L_EMULATION_UP;
    if (out->ly < -0.5f) dirs |= VPAD_STICK_L_EMULATION_DOWN;
    if (out->rx < -0.5f) dirs |= VPAD_STICK_R_EMULATION_LEFT;
    if (out->rx > 0.5f) dirs |= VPAD_STICK_R_EMULATION_RIGHT;
    if (out->ry > 0.5f) dirs |= VPAD_STICK_R_EMULATION_UP;
    if (out->ry < -0.5f) dirs |= VPAD_STICK_R_EMULATION_DOWN;
    out->hold |= dirs;
    out->game_hold |= dirs;
}
