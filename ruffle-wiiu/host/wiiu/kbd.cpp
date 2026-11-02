// The Wii U's system keyboard (nn::swkbd, C++ only) behind a small C interface; see kbd.h.
#include "kbd.h"
extern "C" {
#include "pads.h"
}
#include <coreinit/filesystem.h>
#include <coreinit/memdefaultheap.h>
#include <nn/swkbd.h>
#include <string.h>

extern "C" void plat_log(int level, const char* msg);

static FSClient* g_fs;
static void* g_work;
static bool g_created, g_shown, g_closing;
static char16_t g_initial[256];

int kbd_init(void) {
    if (g_created) return 1;
    g_fs = (FSClient*)MEMAllocFromDefaultHeap(sizeof(FSClient));
    if (!g_fs || FSAddClient(g_fs, FS_ERROR_FLAG_ALL) != FS_STATUS_OK) {
        plat_log(1, "keyboard: no FS client");
        return 0;
    }
    g_work = MEMAllocFromDefaultHeap(nn::swkbd::GetWorkMemorySize(0));
    if (!g_work) {
        plat_log(1, "keyboard: no memory");
        return 0;
    }
    // The region picks which keyboard layouts load; try the common ones.
    static const nn::swkbd::RegionType regions[] = { nn::swkbd::RegionType::USA, nn::swkbd::RegionType::Europe,
                                                     nn::swkbd::RegionType::Japan };
    for (nn::swkbd::RegionType r : regions) {
        nn::swkbd::CreateArg arg;
        arg.workMemory = g_work;
        arg.regionType = r;
        arg.fsClient = g_fs;
        if (nn::swkbd::Create(arg)) {
            g_created = true;
            plat_log(2, "keyboard: ready");
            return 1;
        }
    }
    plat_log(1, "keyboard: couldn't load the system keyboard");
    return 0;
}

// UTF-8 -> UTF-16 (n units including the terminating 0); characters outside the BMP become '?'.
static void to_utf16(const char* s, char16_t* out, size_t n) {
    size_t o = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p && o + 1 < n) {
        unsigned c = *p++;
        if (c >= 0xc0 && c < 0xe0 && (*p & 0xc0) == 0x80) c = (c & 0x1f) << 6 | (*p++ & 0x3f);
        else if (c >= 0xe0 && c < 0xf0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) {
            c = (c & 0x0f) << 12 | (p[0] & 0x3f) << 6 | (p[1] & 0x3f);
            p += 2;
        } else if (c >= 0x80) {
            while ((*p & 0xc0) == 0x80) p++;
            c = '?';
        }
        out[o++] = (char16_t)c;
    }
    out[o] = 0;
}

static void to_utf8(const char16_t* s, char* out, size_t cap) {
    size_t o = 0;
    for (; s && *s; s++) {
        unsigned c = *s;
        if (c >= 0xd800 && c < 0xe000) c = '?';
        size_t need = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
        if (o + need >= cap) break;
        if (need == 1) out[o++] = (char)c;
        else if (need == 2) { out[o++] = (char)(0xc0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3f)); }
        else {
            out[o++] = (char)(0xe0 | c >> 12);
            out[o++] = (char)(0x80 | (c >> 6 & 0x3f));
            out[o++] = (char)(0x80 | (c & 0x3f));
        }
    }
    if (cap) out[o] = 0;
}

void kbd_open(const char* text, int max_len, int password, int multiline) {
    if (!g_created || g_shown) return;
    to_utf16(text ? text : "", g_initial, sizeof(g_initial) / sizeof(g_initial[0]));
    nn::swkbd::AppearArg arg;
    arg.keyboardArg.configArg.languageType = nn::swkbd::LanguageType::English;
    arg.keyboardArg.configArg.disableNewLine = !multiline;
    arg.inputFormArg.initialText = g_initial;
    arg.inputFormArg.maxTextLength = max_len > 0 ? max_len : -1;
    arg.inputFormArg.passwordMode = password ? nn::swkbd::PasswordMode::Fade : nn::swkbd::PasswordMode::Clear;
    if (nn::swkbd::AppearInputForm(arg)) {
        g_shown = true;
        g_closing = false;
    } else {
        plat_log(1, "keyboard: couldn't show it");
    }
}

int kbd_busy(void) {
    return g_shown;
}

int kbd_update(VPADStatus* vs, char* out, size_t cap) {
    if (!g_shown) return 0;
    VPADStatus copy;
    nn::swkbd::ControllerInfo info;
    if (vs) {
        copy = *vs;
        // The keyboard expects touch positions calibrated to the GamePad screen.
        VPADGetTPCalibratedPoint(VPAD_CHAN_0, &copy.tpNormal, &vs->tpNormal);
        info.vpad = &copy;
    }
    // Wii Remotes (and their Nunchuk, Classic or Pro Controller) type on it too, with the pointer as the cursor.
    for (int c = 0; c < 4; c++) info.kpad[c] = (KPADStatus*)pads_kpad(c);
    nn::swkbd::Calc(info);
    if (nn::swkbd::IsNeedCalcSubThreadFont()) nn::swkbd::CalcSubThreadFont();
    if (nn::swkbd::IsNeedCalcSubThreadPredict()) nn::swkbd::CalcSubThreadPredict();
    if (g_closing) {
        // Fading out: the game gets input again once it's gone.
        if (nn::swkbd::GetStateInputForm() == nn::swkbd::State::Hidden) g_shown = false;
        return 0;
    }
    if (nn::swkbd::IsDecideOkButton(nullptr)) {
        to_utf8(nn::swkbd::GetInputFormString(), out, cap);
        nn::swkbd::DisappearInputForm();
        g_closing = true;
        return 1;
    }
    if (nn::swkbd::IsDecideCancelButton(nullptr)) {
        nn::swkbd::DisappearInputForm();
        g_closing = true;
        return 2;
    }
    return 0;
}

void kbd_draw(int drc) {
    if (!g_shown) return;
    if (drc) nn::swkbd::DrawDRC();
    else nn::swkbd::DrawTV();
}

void kbd_cancel(void) {
    if (g_shown && !g_closing) nn::swkbd::DisappearInputForm();
    g_shown = g_closing = false;
}

void kbd_shutdown(void) {
    if (g_created) nn::swkbd::Destroy();
    g_created = g_shown = false;
    if (g_work) MEMFreeToDefaultHeap(g_work);
    g_work = nullptr;
    if (g_fs) {
        FSDelClient(g_fs, FS_ERROR_FLAG_ALL);
        MEMFreeToDefaultHeap(g_fs);
        g_fs = nullptr;
    }
}
