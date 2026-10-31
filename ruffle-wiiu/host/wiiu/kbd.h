// The Wii U's system keyboard (nn::swkbd) on the GamePad, for text fields in games.
#pragma once
#include <stddef.h>
#include <vpad/input.h>

#ifdef __cplusplus
extern "C" {
#endif

// Loads the keyboard (once, at start). Returns 0 if it isn't available.
int kbd_init(void);
// Shows it with `text` (UTF-8) already typed. max_len: 0 = any; password hides the text.
void kbd_open(const char* text, int max_len, int password, int multiline);
// 1 while the keyboard is shown or fading out: the game gets no GamePad input then.
int kbd_busy(void);
// Each frame while busy, with the frame's GamePad sample (NULL if none): 1 = OK pressed, the text is
// in out (UTF-8); 2 = cancelled; 0 = still typing (or fading out).
int kbd_update(VPADStatus* vs, char* out, size_t cap);
// Draws the keyboard over the TV (drc 0) or GamePad (drc 1) picture.
void kbd_draw(int drc);
// Takes it away at once (leaving the game).
void kbd_cancel(void);
void kbd_shutdown(void);

#ifdef __cplusplus
}
#endif
