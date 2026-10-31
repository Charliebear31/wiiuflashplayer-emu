#pragma once
#include <stdint.h>

// Call after WHBGfxInit. Ruffle's screen target is width x height.
int gfx_gx2_init(uint32_t width, uint32_t height);
// Shows the screen target on the TV and the GamePad, then frees GPU memory Ruffle released.
void gfx_gx2_present(void);
// GPU memory in use (MB) and in how many blocks, for the log.
void gfx_gx2_memory(uint32_t* mb, uint32_t* blocks);
// Test scripts: run Flash filters on the CPU instead of the GPU (to compare the two).
void gfx_gx2_filters_on_cpu(int on);
// A small RGBA picture over the top-left corner (the FPS counter); same size every call. NULL hides it.
void gfx_gx2_set_hud(const uint8_t* rgba, uint32_t w, uint32_t h);
// The mouse arrow: its picture (with transparent corners), then where it is each frame, in screen-texture pixels.
void gfx_gx2_set_cursor(const uint8_t* rgba, uint32_t w, uint32_t h);
void gfx_gx2_set_cursor_pos(int on, float x, float y);
// Pictures over the game (the on-screen keyboard and its tab), slot 0 or 1, at (x, y) in screen-texture pixels.
void gfx_gx2_set_panel(int slot, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t stride, float x, float y);
void gfx_gx2_panel_on(int slot, int on);
// Drawn over the TV (drc 0) and GamePad (drc 1) picture each present, e.g. the system keyboard; NULL = none.
void gfx_gx2_set_overlay(void (*draw)(int drc));
// The system asked the app to exit: stop waiting on, reading from and drawing with the GPU.
void gfx_gx2_gpu_lost(void);
// RGBA8, top row first.
void gfx_gx2_read_screen(uint8_t* rgba);
void gfx_gx2_read_screen_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* rgba);
// Shows rgba (screen-sized RGBA8 art) around the game, with the game's stage rect (x, y, w, h in screen
// pixels) on top. NULL turns it off.
void gfx_gx2_set_border(const uint8_t* rgba, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
// Brightness of everything shown (0 black .. 1 normal), for fades; applied at present time on the GPU.
void gfx_gx2_set_fade(float brightness);
// Replaces the screen target with an RGBA8 image of the same size (the game picker).
void gfx_tex_update_screen(const uint8_t* rgba);
// The same for just these areas (x, y, w, h) of the 1280x720 canvas.
void gfx_tex_update_screen_rects(const uint8_t* rgba, const int (*rects)[4], int n);
void gfx_gx2_screen_size(uint32_t* w, uint32_t* h);
