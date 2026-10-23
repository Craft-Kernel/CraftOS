#pragma once
#include <stdbool.h>
#include <stdint.h>

#define GFX_WIDTH  640
#define GFX_HEIGHT 480

#define FONT_W 8
#define FONT_H 16

/*
 * a 640x480 screen with 32 bits per pixel. everything is drawn into a back
 * buffer in ordinary memory first, and gfx_present() copies the finished
 * picture to the screen in one go, so nothing ever flickers. the mouse
 * pointer is drawn straight onto the screen on top, so moving it doesn't need
 * everything redrawn.
 *
 * colours are written 0xrrggbb.
 */
bool gfx_init(void);

/*
 * back to the 80x25 text screen, with the vga font put back. graphics will
 * have scribbled all over the text screen's memory, so clear it before
 * printing anything.
 */
void gfx_exit(void);
bool gfx_active(void);

/* keeps all drawing inside a rectangle until gfx_unclip(). */
void gfx_clip(int x, int y, int w, int h);
void gfx_unclip(void);

void gfx_fill(int x, int y, int w, int h, uint32_t color);
void gfx_rect(int x, int y, int w, int h, uint32_t color);
void gfx_char(int x, int y, char c, uint32_t color);
void gfx_text(int x, int y, const char *s, uint32_t color);
void gfx_textn(int x, int y, const char *s, int max_chars, uint32_t color);

/*
 * the two-tone 3d border: raised for buttons and windows, sunken for boxes
 * and pressed buttons.
 */
void gfx_bevel(int x, int y, int w, int h, bool raised);

/*
 * draws a picture with its top left corner at (x, y), mixing any see-through
 * pixels with what's already there. the scaled version stretches it to w by
 * h, for things like the image viewer.
 */
struct image;
void gfx_blit(int x, int y, const struct image *img);
void gfx_blit_scaled(int x, int y, int w, int h, const struct image *img);

enum { CURSOR_ARROW, CURSOR_RESIZE_H, CURSOR_RESIZE_V, CURSOR_RESIZE_D };

void gfx_present(int cursor_x, int cursor_y);
void gfx_move_cursor(int x, int y);
void gfx_set_cursor(int shape);
