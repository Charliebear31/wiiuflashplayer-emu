#pragma once
#include <stdint.h>

#define UI_W 1280
#define UI_H 720

typedef struct {
    char path[256];  // WASI path, e.g. "/wiiu/apps/flashplayeremu/swf/game.swf"
    char title[64];  // shown name
    int version;     // SWF version, 0 if unknown
    uint32_t size;   // bytes
    uint8_t* thumb;  // UI_THUMB_W x UI_THUMB_H RGBA8 picture of the game, or NULL
    // A folder tile instead of a game: title is the folder's name, count its games, sub the pictures of
    // the first four (NULL where none).
    int folder;
    int count;
    const uint8_t* sub[4];
} UiGame;

// Game pictures are captured from the game itself (main_wiiu.c) at twice this size, then halved.
#define UI_THUMB_W 200
#define UI_THUMB_H 150

// Fills title/version/size from the file (sd path = "fs:/vol/external01" + path).
void ui_game_info(UiGame* g, const char* sd_root);

// Draws the game picker into an RGBA8 UI_W x UI_H canvas.
// t: seconds since the picker opened; level: music loudness 0..1;
// launch: 0, or 0..1 through the "starting a game" animation.
void ui_draw_picker(uint8_t* canvas, const UiGame* games, int n, int sel, float t, float level, float launch);

// "Taking game pictures" screen shown while main_wiiu.c captures them.
void ui_draw_progress(uint8_t* canvas, int done, int total, const char* name, float t);

// Shown while a game starts: big files take many seconds to unpack before anything can be drawn.
void ui_draw_loading(uint8_t* canvas, const char* name);

// Art around a game that doesn't fill the 16:9 screen; (sx, sy, sw, sh) is the game's stage rect.
void ui_draw_border(uint8_t* canvas, int sx, int sy, int sw, int sh);

// Settings panel drawn over the picker: n rows of label + value (value "" for actions), sel highlighted.
#define UI_SETTINGS_MAX 8
// How the menus look.
enum { UI_THEME_CLASSIC, UI_THEME_FLASH, UI_THEME_HALLOWEEN, UI_THEME_COUNT };
void ui_set_theme(int theme);
// The mouse arrow (RGBA8; its tip is the top left corner).
#define UI_CURSOR_W 24
#define UI_CURSOR_H 32
void ui_cursor_pixels(uint8_t* rgba);
// The on-screen keyboard in games, in the theme's colours: a tab on the far left opens it. Both are drawn into a
// UI_W x UI_H canvas (the keyboard at its top left, the tab at UI_OSK_TAB_CANVAS_Y) and shown as pictures over the game.
#define UI_OSK_W 768
#define UI_OSK_H 240
#define UI_OSK_X 256
#define UI_OSK_Y 472
#define UI_OSK_TAB_W 44
#define UI_OSK_TAB_H 52
#define UI_OSK_TAB_X 0
#define UI_OSK_TAB_Y 664
#define UI_OSK_TAB_CANVAS_Y 300
#define UI_OSK_KEYS 49
void ui_draw_osk(uint8_t* canvas, const uint8_t* held);
void ui_draw_osk_tab(uint8_t* canvas, int open);
int ui_osk_key_at(int x, int y);   // key under a screen position, or -1
uint32_t ui_osk_code(int key);     // what the key sends; 0: closes the keyboard
void ui_draw_settings(uint8_t* canvas, const char* const* labels, const char* const* values, int n, int sel, float t);
// Settings row under a touch, or -1.
int ui_settings_row_at(int n, int x, int y);
// Credits page drawn over the picker.
void ui_draw_credits(uint8_t* canvas, float t);
// The SETTINGS button on the picker's tab row was touched.
int ui_settings_button_at(int x, int y);

// The canvas areas changed by the last ui_draw_* call, as x, y, w, h (one full-screen rect when
// everything changed); returns how many. Only those need uploading.
int ui_take_dirty(int (*rects)[4], int max);
// The screen texture was drawn over by something else (a game): the next frame uploads everything.
void ui_forget_screen(void);

// Games are tiles in a grid UI_COLS wide (left/right move by 1, up/down by UI_COLS), UI_ROWS on screen.
#define UI_COLS 4
#define UI_ROWS 2

// Tile under a GamePad touch (1280x720 coordinates), or -1.
int ui_tile_at(int n, int x, int y);
// The tile whose picture's middle is under (x, y) (dropping a game there makes a folder), or -1.
int ui_tile_middle_at(int n, int x, int y);
// Where a game dropped at (x, y) goes in the list of n (0..n; before the tile under it, or after it when
// on its right half), or -1 off the grid.
int ui_insert_at(int n, int x, int y);

// Inside a folder: its name heads the page, with "back" and "rename" buttons (NULL: the main list).
void ui_set_folder(const char* name);
int ui_back_button_at(int x, int y);   // (the rename button is where the settings one is)
// Dragging a tile: `index` is the dragged tile (drawn under the finger at x, y, its place left empty),
// `target` a tile highlighted as a folder drop (or -1), `insert` where it would go (or -1),
// `on_back` the back button highlighted. index -1: not dragging.
void ui_set_drag(int index, int x, int y, int target, int insert, int on_back);
// The first row of tiles on screen; scrolls the grid of n tiles (dragging near its top / bottom).
int ui_top_row(void);
void ui_set_top_row(int row, int n);
// Row of games to scroll to for a touch on the scroll bar (shown when the games don't fit), or -1.
// first: the touch just started (a drag that began on the bar keeps scrolling while held).
int ui_scroll_at(int n, int x, int y, int first);
