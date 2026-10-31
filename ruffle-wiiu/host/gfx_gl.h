#pragma once
#include <stdint.h>

// Call with a current GL context. The screen target is width x height.
int gfx_gl_init(uint32_t width, uint32_t height);
void gfx_gl_resize(uint32_t width, uint32_t height);
// Copies the screen target into the window's default framebuffer.
void gfx_gl_present(int win_w, int win_h);
// RGBA8, top row first.
void gfx_gl_read_screen(uint8_t* rgba);
