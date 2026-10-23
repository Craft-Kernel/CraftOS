#include <apps.h>
#include <console.h>
#include <cpu.h>
#include <gfx.h>
#include <gui.h>
#include <image.h>
#include <io.h>
#include <string.h>
#include <task.h>
#include <version.h>

/*
 * the craftos desktop: a teal background with a column of icons down the
 * side, overlapping windows, and a taskbar with a start menu and a clock. the
 * apps themselves each live in their own file, and plug in here by filling
 * out a struct window.
 *
 * you can drag a window around by its title bar, resize it from the right and
 * bottom edges, minimize it down to the taskbar, maximize it to fill the
 * desktop, or press f11 to make it full screen.
 *
 * there's no clever tracking of which bits of the screen changed. whenever
 * anything changes, the screen is marked dirty, the whole lot is redrawn into
 * the back buffer, and the back buffer gets copied to the screen. at 640x480
 * that's cheap enough that nobody notices.
 */

#define C_LOGO_WOOD 0xD9A35B
#define C_LOGO_DARK 0x6B3E1E

static bool inside(int px, int py, int x, int y, int w, int h)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

/* ---- shared drawing pieces ---- */

void ui_text_bold(int x, int y, const char *s, uint32_t color)
{
    gfx_text(x, y, s, color);
    gfx_text(x + 1, y, s, color);
}

/* the text calls the newer apps make, all drawn in the classic vga font */
int ui_text_fit(int x, int y, const char *s, int max_w, uint32_t color, bool bold)
{
    int max_chars = max_w > 0 ? max_w / FONT_W : 1 << 30;
    gfx_textn(x, y, s, max_chars, color);
    if (bold)
        gfx_textn(x + 1, y, s, max_chars, color);
    int len = strlen(s);
    return (len < max_chars ? len : max_chars) * FONT_W;
}

void ui_text(int x, int y, const char *s, uint32_t color)
{
    gfx_text(x, y, s, color);
}

int ui_text_width(const char *s, bool bold)
{
    (void)bold;
    return (int)strlen(s) * FONT_W;
}

/* any task can set this to ask the desktop's loop for a redraw */
static volatile bool poked;

void gui_poke(void)
{
    poked = true;
}

/*
 * buttons: a grey face with a raised bevel, sunk in while they're pressed.
 * the default button (the one enter presses) gets a black outline too, so you
 * can tell which one it is.
 */
void ui_button_ex(int x, int y, int w, int h, const char *label, bool pressed, bool is_default)
{
    gfx_fill(x, y, w, h, C_FACE);
    if (is_default) {
        gfx_rect(x, y, w, h, C_BLACK);
        gfx_bevel(x + 1, y + 1, w - 2, h - 2, !pressed);
    } else {
        gfx_bevel(x, y, w, h, !pressed);
    }
    int tx = x + (w - (int)strlen(label) * FONT_W) / 2 + pressed;
    int ty = y + (h - FONT_H) / 2 + pressed;
    gfx_text(tx, ty, label, C_BLACK);
}

void ui_button(int x, int y, int w, int h, const char *label, bool pressed)
{
    ui_button_ex(x, y, w, h, label, pressed, false);
}

/*
 * pictures the desktop loads off the ram disk the first time it starts. any
 * that are missing just stay null, and whatever wanted them draws something
 * simple instead, so the desktop still works without them.
 */
static struct image *wallpaper, *logo_16, *logo_32, *logo_48;

static const char *const status_names[] = { "info", "question", "warning", "fail" };
static const int status_sizes[] = { 16, 32, 40, 48 };
static struct image *status_icons[4][4];

/*
 * loads the pictures the desktop uses, but only once: after the first time,
 * the desktop keeps them around between visits. the status icons are found by
 * name and size, like /usr/share/icons/status/info-32.
 */
static void load_images(void)
{
    static bool loaded;
    if (loaded)
        return;
    loaded = true;
    wallpaper = image_load("/usr/share/wallpapers/craft.cimg");
    logo_16 = image_load("/usr/share/icons/craft-16.cimg");
    logo_32 = image_load("/usr/share/icons/craft-32.cimg");
    logo_48 = image_load("/usr/share/icons/craft-48.cimg");

    char path[64];
    for (int i = 0; i < 4; i++) {
        for (int s = 0; s < 4; s++) {
            ksnprintf(path, sizeof(path), "/usr/share/icons/status/%s-%d.cimg", status_names[i], status_sizes[s]);
            status_icons[i][s] = image_load(path);
        }
    }
}

/*
 * draws a status icon. if its picture is missing, it draws a plain coloured
 * square with a letter on it instead.
 */
void ui_status_icon(int icon, int x, int y, int size)
{
    for (int s = 0; s < 4; s++) {
        if (status_sizes[s] == size && status_icons[icon][s]) {
            gfx_blit(x, y, status_icons[icon][s]);
            return;
        }
    }
    static const uint32_t colors[] = { 0x1F9A45, 0x1D5FD0, 0xFFD000, 0xD01F12 };
    static const char marks[] = "i?!x";
    gfx_fill(x, y, size, size, colors[icon]);
    gfx_bevel(x, y, size, size, true);
    gfx_char(x + (size - FONT_W) / 2, y + (size - FONT_H) / 2, marks[icon], icon == ICON_WARNING ? C_BLACK : C_WHITE);
}

/*
 * the craftos logo, which is the green craft cube, at 16, 32 or 48 pixels. it
 * turns up on the start button, in the start menu and on the welcome window.
 * if the icon files are missing, it draws a little 3x3 crafting grid instead.
 */
void ui_logo(int x, int y, int size)
{
    struct image *img = size == 16 ? logo_16 : size == 32 ? logo_32 : size == 48 ? logo_48 : 0;
    if (img) {
        gfx_blit(x, y, img);
        return;
    }
    gfx_fill(x, y, size, size, C_LOGO_WOOD);
    gfx_rect(x, y, size, size, C_LOGO_DARK);
    for (int i = 1; i < 3; i++) {
        gfx_fill(x + size * i / 3, y, 1, size, C_LOGO_DARK);
        gfx_fill(x, y + size * i / 3, size, 1, C_LOGO_DARK);
    }
}

/* the little 16x14 icons used in folder and file lists. */
void ui_folder_icon(int x, int y)
{
    gfx_fill(x, y + 1, 7, 3, 0xE0B030);
    gfx_fill(x, y + 3, 16, 11, 0xFFD84A);
    gfx_rect(x, y + 3, 16, 11, 0x8A6A10);
}

/* a tiny framed picture: blue sky, a sun and a green hill. */
void ui_image_icon(int x, int y)
{
    gfx_fill(x + 1, y + 1, 14, 12, 0x80C8FF);
    gfx_fill(x + 9, y + 3, 3, 3, 0xFFE040);
    gfx_fill(x + 2, y + 9, 12, 3, 0x40A040);
    gfx_fill(x + 5, y + 7, 6, 2, 0x40A040);
    gfx_rect(x + 1, y + 1, 14, 12, C_BLACK);
}

void ui_file_icon(int x, int y)
{
    gfx_fill(x + 2, y, 12, 14, C_WHITE);
    gfx_rect(x + 2, y, 12, 14, C_BLACK);
    for (int ly = y + 3; ly < y + 12; ly += 3)
        gfx_fill(x + 4, ly, 8, 1, C_SHADOW);
}

/* ---- scrollbars ---- */

/*
 * scrollbars have an arrow button at each end and a track in between. the
 * thumb's length shows how much of the content fits on screen at once, and
 * where it sits in the track shows how far down you've scrolled.
 */

int ui_scroll_max(const struct scrollbar *sb)
{
    return sb->total > sb->visible ? sb->total - sb->visible : 0;
}

void ui_scroll_by(struct scrollbar *sb, int rows)
{
    int max = ui_scroll_max(sb);
    sb->offset += rows;
    if (sb->offset > max)
        sb->offset = max;
    if (sb->offset < 0)
        sb->offset = 0;
}

static int thumb_len(const struct scrollbar *sb, int track)
{
    int len = sb->total ? track * sb->visible / sb->total : track;
    if (len < 10)
        len = 10;
    return len > track ? track : len;
}

static int thumb_pos(const struct scrollbar *sb, int track)
{
    int max = ui_scroll_max(sb);
    return SCROLLBAR_W + (max ? (track - thumb_len(sb, track)) * sb->offset / max : 0);
}

static void arrow(int cx, int cy, bool up, uint32_t color)
{
    for (int i = 0; i < 4; i++) {
        int row = up ? cy - 2 + i : cy + 1 - i;
        gfx_fill(cx - i, row, 1 + 2 * i, 1, color);
    }
}

/*
 * draws a scrollbar h pixels tall at (x, y): the track, the two arrow
 * buttons, and the thumb. when everything already fits, the arrows go grey
 * and there's no thumb at all, since there's nowhere to scroll to.
 */
void ui_scrollbar(int x, int y, int h, struct scrollbar *sb)
{
    bool active = ui_scroll_max(sb) > 0;
    uint32_t arrow_color = active ? C_BLACK : C_SHADOW;
    int track = h - 2 * SCROLLBAR_W;

    gfx_fill(x, y, SCROLLBAR_W, h, 0xE8E8E8);

    gfx_fill(x, y, SCROLLBAR_W, SCROLLBAR_W, C_FACE);
    gfx_bevel(x, y, SCROLLBAR_W, SCROLLBAR_W, true);
    arrow(x + 7, y + 7, true, arrow_color);

    gfx_fill(x, y + h - SCROLLBAR_W, SCROLLBAR_W, SCROLLBAR_W, C_FACE);
    gfx_bevel(x, y + h - SCROLLBAR_W, SCROLLBAR_W, SCROLLBAR_W, true);
    arrow(x + 7, y + h - SCROLLBAR_W + 7, false, arrow_color);

    if (active && track > 0) {
        int ty = y + thumb_pos(sb, track);
        int tl = thumb_len(sb, track);
        gfx_fill(x, ty, SCROLLBAR_W, tl, C_FACE);
        gfx_bevel(x, ty, SCROLLBAR_W, tl, true);
    }
}

bool ui_scrollbar_click(struct scrollbar *sb, int y, int h)
{
    int track = h - 2 * SCROLLBAR_W;
    sb->grab = -1;

    if (y < SCROLLBAR_W) {
        ui_scroll_by(sb, -1);
    } else if (y >= h - SCROLLBAR_W) {
        ui_scroll_by(sb, 1);
    } else if (ui_scroll_max(sb) > 0) {
        int pos = thumb_pos(sb, track);
        if (y < pos)
            ui_scroll_by(sb, -sb->visible);
        else if (y >= pos + thumb_len(sb, track))
            ui_scroll_by(sb, sb->visible);
        else
            sb->grab = y - pos;
    }
    return true;
}

bool ui_scrollbar_drag(struct scrollbar *sb, int y, int h)
{
    if (sb->grab < 0)
        return false;
    int track = h - 2 * SCROLLBAR_W;
    int room = track - thumb_len(sb, track);
    if (room <= 0)
        return false;
    sb->offset = (y - sb->grab - SCROLLBAR_W) * ui_scroll_max(sb) / room;
    ui_scroll_by(sb, 0);
    return true;
}

bool ui_scroll_key(struct scrollbar *sb, char c)
{
    switch (c) {
    case KEY_UP:   ui_scroll_by(sb, -1); return true;
    case KEY_DOWN: ui_scroll_by(sb, 1); return true;
    case KEY_PGUP: ui_scroll_by(sb, -sb->visible); return true;
    case KEY_PGDN: ui_scroll_by(sb, sb->visible); return true;
    default:       return false;
    }
}

/* ---- the two small windows that live here ---- */

static void draw_welcome(struct window *win, int cx, int cy, int cw, int ch)
{
    (void)win, (void)cw, (void)ch;
    ui_logo(cx + 6, cy + 6, 48);
    ui_text_bold(cx + 62, cy + 14, "Welcome to CraftOS!", C_BLACK);
    gfx_text(cx + 62, cy + 32, "A tiny desktop on the Craft Kernel.", C_BLACK);

    static const char *const tips[] = {
        "- Drag edges to resize, F11 for full screen.",
        "- Double-click an icon to open it.",
        "- Explore / in Files or the Terminal.",
        "- Ctrl+Alt+Del opens the Task Manager.",
    };
    for (int i = 0; i < 4; i++)
        gfx_text(cx + 12, cy + 70 + i * 20, tips[i], C_BLACK);
}

static void draw_system(struct window *win, int cx, int cy, int cw, int ch)
{
    (void)win, (void)cw, (void)ch;
    char line[64];

    ui_status_icon(ICON_INFO, cw - 32 - 10 + cx, cy + 8, 32);
    ui_text_bold(cx + 10, cy + 8, OS_NAME " " OS_VERSION, C_BLACK);
    gfx_text(cx + 10, cy + 28, "Kernel:  " KERNEL_NAME, C_BLACK);

    ksnprintf(line, sizeof(line), "Memory:  %u MiB", system_mem_kib() / 1024);
    gfx_text(cx + 10, cy + 46, line, C_BLACK);

    gfx_text(cx + 10, cy + 64, "Display: 640 x 480, 32-bit", C_BLACK);

    uint32_t up = timer_ticks() / timer_hz();
    ksnprintf(line, sizeof(line), "Uptime:  %u:%02u:%02u", up / 3600, up / 60 % 60, up % 60);
    gfx_text(cx + 10, cy + 82, line, C_BLACK);
}

static struct window welcome_window = {
    .title = "Welcome", .proc = "welcome", .x = 130, .y = 60, .w = 400, .h = 210,
    .draw = draw_welcome,
};

static struct window system_window = {
    .title = "System", .proc = "system", .x = 260, .y = 190, .w = 300, .h = 150,
    .draw = draw_system,
};

/* ---- windows ---- */

/*
 * windows[] is every window in a fixed order, and it's what the taskbar goes
 * by. order[] is how they're stacked, from the back to the front. the
 * frontmost window that's open and not minimized is the one with the focus.
 */

static struct window *const windows[] = {
    &files_window, &terminal_window, &notepad_window, &taskman_window, &system_window, &welcome_window,
    &mines_window, &imgview_window, &dialog_window,
};

#define WIN_COUNT ((int)(sizeof(windows) / sizeof(windows[0])))

static struct window *order[WIN_COUNT] = {
    &dialog_window, &imgview_window, &mines_window,
    &system_window, &taskman_window, &notepad_window, &terminal_window, &files_window, &welcome_window,
};

static bool visible(struct window *win)
{
    return win->open && !win->minimized;
}

static struct window *focused(void)
{
    for (int i = WIN_COUNT - 1; i >= 0; i--)
        if (visible(order[i]))
            return order[i];
    return 0;
}

static void raise(struct window *win)
{
    int i = 0;
    while (order[i] != win)
        i++;
    for (; i < WIN_COUNT - 1; i++)
        order[i] = order[i + 1];
    order[WIN_COUNT - 1] = win;
}

/*
 * opening a window also adds it as a task, so it shows up in ps and the task
 * manager. closing it, whether from its close button or with kill from the
 * shell or task manager, takes it off the list again.
 */
static void close_from_task(void *ctx)
{
    gui_close(ctx);
}

void gui_open(struct window *win)
{
    raise(win);
    if (!win->open) {
        int pid = task_add_app(win->proc, close_from_task, win);
        win->pid = pid > 0 ? pid : 0;
    }
    win->open = true;
    win->minimized = false;
}

void gui_close(struct window *win)
{
    if (win->pid)
        task_remove(win->pid);
    win->pid = 0;
    win->open = false;
    win->fullscreen = false;
}

/*
 * maximizing remembers where the window was, so restoring it can put it back
 * exactly where it came from. full screen is a separate thing: it covers the
 * taskbar too and hides the window's frame, but doesn't touch the window's
 * own position at all.
 */
static void toggle_maximize(struct window *win)
{
    if (!win->resizable)
        return;
    if (win->maximized) {
        win->x = win->normal_x;
        win->y = win->normal_y;
        win->w = win->normal_w;
        win->h = win->normal_h;
        win->maximized = false;
    } else {
        win->normal_x = win->x;
        win->normal_y = win->y;
        win->normal_w = win->w;
        win->normal_h = win->h;
        win->x = 0;
        win->y = 0;
        win->w = GFX_WIDTH;
        win->h = DESK_H;
        win->maximized = true;
    }
}

static uint32_t fullscreen_since;

static void toggle_fullscreen(struct window *win)
{
    if (!win->resizable)
        return;
    win->fullscreen = !win->fullscreen;
    fullscreen_since = timer_ticks();
}

/*
 * the window frame: a raised grey border, a navy title bar (grey when the
 * window doesn't have the focus), and the title bar buttons from right to
 * left: close, then maximize (only if the window can be resized), then
 * minimize.
 */

#define TB_W 16
#define TB_H 14

static void close_box(struct window *win, int *x, int *y)
{
    *x = win->x + win->w - 22;
    *y = win->y + 5;
}

static void max_box(struct window *win, int *x, int *y)
{
    *x = win->x + win->w - 40;
    *y = win->y + 5;
}

static void min_box(struct window *win, int *x, int *y)
{
    *x = win->x + win->w - (win->resizable ? 56 : 40);
    *y = win->y + 5;
}

static void content_rect(struct window *win, int *x, int *y, int *w, int *h)
{
    if (win->fullscreen) {
        *x = 0;
        *y = 0;
        *w = GFX_WIDTH;
        *h = GFX_HEIGHT;
    } else {
        *x = win->x + 4;
        *y = win->y + 4 + TITLE_H + 2;
        *w = win->w - 8;
        *h = win->h - TITLE_H - 10;
    }
    win->cw = *w;
    win->ch = *h;
}

static void title_button(int x, int y)
{
    gfx_fill(x, y, TB_W, TB_H, C_FACE);
    gfx_bevel(x, y, TB_W, TB_H, true);
}

/*
 * draws the buttons on a window's title bar, each one a little grey button
 * with a black symbol. the maximize button shows two overlapping boxes while
 * the window is maximized (meaning "restore"), and one big box otherwise.
 * dialogs only get a close button.
 */
static void draw_title_buttons(struct window *win)
{
    int bx, by;

    /* close: an x drawn two pixels thick */
    close_box(win, &bx, &by);
    title_button(bx, by);
    for (int i = 0; i < 6; i++) {
        gfx_fill(bx + 4 + i, by + 4 + i, 2, 1, C_BLACK);
        gfx_fill(bx + 10 - i, by + 4 + i, 2, 1, C_BLACK);
    }

    if (win->resizable) {
        max_box(win, &bx, &by);
        title_button(bx, by);
        if (win->maximized) {
            gfx_rect(bx + 6, by + 2, 7, 6, C_BLACK);
            gfx_fill(bx + 6, by + 3, 7, 1, C_BLACK);
            gfx_fill(bx + 3, by + 5, 7, 6, C_FACE);
            gfx_rect(bx + 3, by + 5, 7, 6, C_BLACK);
            gfx_fill(bx + 3, by + 6, 7, 1, C_BLACK);
        } else {
            gfx_rect(bx + 3, by + 2, 9, 9, C_BLACK);
            gfx_fill(bx + 3, by + 3, 9, 1, C_BLACK);
        }
    }

    if (win->modal)
        return;
    min_box(win, &bx, &by);
    title_button(bx, by);
    gfx_fill(bx + 4, by + 9, 6, 2, C_BLACK);
}

/*
 * draws one window: the raised grey frame and the title bar (navy when it has
 * the focus) with its title and buttons, then the app's own content. the
 * title gets cut short so it never runs under the buttons. everything is
 * clipped, first to the window and then to its content area, so an app can't
 * scribble outside its own space. a full screen window skips the frame and
 * just draws its content over the whole screen.
 */
static void draw_window(struct window *win, bool active)
{
    int cx, cy, cw, ch;
    content_rect(win, &cx, &cy, &cw, &ch);

    if (!win->fullscreen) {
        int x = win->x, y = win->y, w = win->w, h = win->h;
        gfx_clip(x, y, w, h);

        gfx_fill(x, y, w, h, C_FACE);
        gfx_bevel(x, y, w, h, true);

        uint32_t text = active ? C_WHITE : C_FACE;
        int buttons = win->modal ? 1 : win->resizable ? 3 : 2;
        int max_chars = (w - 16 - buttons * 18) / FONT_W;
        gfx_fill(x + 3, y + 3, w - 6, TITLE_H, active ? C_SELECT : C_SHADOW);
        gfx_textn(x + 7, y + 4, win->title, max_chars, text);
        gfx_textn(x + 8, y + 4, win->title, max_chars, text);
        draw_title_buttons(win);
    }

    gfx_clip(cx, cy, cw, ch);
    win->draw(win, cx, cy, cw, ch);
    gfx_unclip();
}

/*
 * which edges of a window a point is on, for resizing: bit 0 means the right
 * edge and bit 1 the bottom. the bottom right corner counts as both, and it
 * gets a bigger area to grab since it's the handiest one to use.
 */
enum { EDGE_RIGHT = 1, EDGE_BOTTOM = 2 };

static int resize_edges(struct window *win, int px, int py)
{
    if (!win->resizable || win->maximized || win->fullscreen)
        return 0;
    if (!inside(px, py, win->x, win->y, win->w, win->h))
        return 0;

    int right = win->x + win->w, bottom = win->y + win->h;
    if (px >= right - 12 && py >= bottom - 12)
        return EDGE_RIGHT | EDGE_BOTTOM;
    int edges = 0;
    if (px >= right - 4)
        edges |= EDGE_RIGHT;
    if (py >= bottom - 4)
        edges |= EDGE_BOTTOM;
    return edges;
}

static int edge_cursor(int edges)
{
    switch (edges) {
    case EDGE_RIGHT:               return CURSOR_RESIZE_H;
    case EDGE_BOTTOM:              return CURSOR_RESIZE_V;
    case EDGE_RIGHT | EDGE_BOTTOM: return CURSOR_RESIZE_D;
    default:                       return CURSOR_ARROW;
    }
}

/* the frontmost window that's showing under a point, if there is one. */
static struct window *window_at(int px, int py)
{
    struct window *top = focused();
    if (top && top->fullscreen)
        return top;
    for (int i = WIN_COUNT - 1; i >= 0; i--)
        if (visible(order[i]) && inside(px, py, order[i]->x, order[i]->y, order[i]->w, order[i]->h))
            return order[i];
    return 0;
}

/* ---- desktop icons ---- */

/*
 * the desktop icons sit in columns down the left side. a single click selects
 * one, and a double click opens its window.
 */

#define ICON_W 72
#define ICON_H 64

static void icon_computer(int x, int y)
{
    gfx_fill(x + 2, y + 2, 28, 20, C_FACE);
    gfx_rect(x + 2, y + 2, 28, 20, C_BLACK);
    gfx_fill(x + 5, y + 5, 22, 14, C_DESKTOP);
    gfx_fill(x + 12, y + 22, 8, 3, C_SHADOW);
    gfx_fill(x + 6, y + 25, 20, 5, C_FACE);
    gfx_rect(x + 6, y + 25, 20, 5, C_BLACK);
}

static void icon_notepad(int x, int y)
{
    gfx_fill(x + 6, y + 1, 20, 29, C_WHITE);
    gfx_rect(x + 6, y + 1, 20, 29, C_BLACK);
    for (int ly = y + 7; ly < y + 27; ly += 4)
        gfx_fill(x + 9, ly, 14, 1, C_SHADOW);
}

static void icon_folder(int x, int y)
{
    gfx_fill(x + 2, y + 4, 12, 5, 0xE0B030);
    gfx_fill(x + 2, y + 8, 28, 20, 0xFFD84A);
    gfx_rect(x + 2, y + 8, 28, 20, 0x8A6A10);
    gfx_fill(x + 3, y + 9, 26, 1, 0xFFF0A0);
}

static void icon_terminal(int x, int y)
{
    gfx_fill(x + 1, y + 2, 30, 24, C_FACE);
    gfx_rect(x + 1, y + 2, 30, 24, C_BLACK);
    gfx_fill(x + 4, y + 5, 24, 18, C_BLACK);
    gfx_char(x + 6, y + 6, '>', C_FACE);
    gfx_char(x + 14, y + 6, '_', C_FACE);
}

static void icon_welcome(int x, int y)
{
    ui_logo(x, y, 32);
}

/* a little window with a bar chart in it. */
static void icon_taskman(int x, int y)
{
    gfx_fill(x + 1, y + 2, 30, 26, C_FACE);
    gfx_rect(x + 1, y + 2, 30, 26, C_BLACK);
    gfx_fill(x + 2, y + 3, 28, 5, C_SELECT);
    gfx_fill(x + 4, y + 10, 24, 15, C_BLACK);
    static const int bars[] = { 6, 11, 4, 9, 13 };
    for (int i = 0; i < 5; i++)
        gfx_fill(x + 6 + i * 4, y + 24 - bars[i], 3, bars[i], 0x00C000);
}

/* a black mine with spikes and a shine on it, on a grey tile */
static void icon_mines(int x, int y)
{
    gfx_fill(x + 2, y + 2, 28, 28, C_FACE);
    gfx_bevel(x + 2, y + 2, 28, 28, true);
    gfx_fill(x + 15, y + 6, 2, 20, C_BLACK);
    gfx_fill(x + 6, y + 15, 20, 2, C_BLACK);
    gfx_fill(x + 10, y + 10, 12, 12, C_BLACK);
    gfx_fill(x + 9, y + 12, 14, 8, C_BLACK);
    gfx_fill(x + 12, y + 9, 8, 14, C_BLACK);
    gfx_fill(x + 12, y + 12, 3, 3, C_WHITE);
}

static const struct {
    const char *label;
    struct window *win;
    void (*draw)(int x, int y);
} icons[] = {
    { "Files", &files_window, icon_folder },
    { "Terminal", &terminal_window, icon_terminal },
    { "Notepad", &notepad_window, icon_notepad },
    { "Tasks", &taskman_window, icon_taskman },
    { "System", &system_window, icon_computer },
    { "Welcome", &welcome_window, icon_welcome },
    { "Mines", &mines_window, icon_mines },
};

#define ICON_COUNT ((int)(sizeof(icons) / sizeof(icons[0])))

static int selected_icon = -1;

/* down the left in a column, then a second column once the first one's full */
#define ICON_ROWS ((DESK_H - 12) / (ICON_H + 8))

static void icon_pos(int i, int *x, int *y)
{
    *x = 8 + i / ICON_ROWS * ICON_W;
    *y = 12 + i % ICON_ROWS * (ICON_H + 8);
}

static void draw_icons(void)
{
    for (int i = 0; i < ICON_COUNT; i++) {
        int x, y;
        icon_pos(i, &x, &y);
        icons[i].draw(x + (ICON_W - 32) / 2, y);

        int len = strlen(icons[i].label);
        int tx = x + (ICON_W - len * FONT_W) / 2, ty = y + 36;
        if (i == selected_icon)
            gfx_fill(tx - 2, ty, len * FONT_W + 4, FONT_H, C_SELECT);
        else
            gfx_text(tx + 1, ty + 1, icons[i].label, C_BLACK);
        gfx_text(tx, ty, icons[i].label, C_WHITE);
    }
}

/* ---- start menu ---- */

#define MENU_W  180
#define ITEM_H  26
#define SEP_H   8
#define BAND_W  24

enum { ACT_OPEN, ACT_SEPARATOR, ACT_TEXT_MODE, ACT_RESTART, ACT_SHUTDOWN };

static const struct {
    const char *label;
    int action;
    struct window *win;
} menu_items[] = {
    { "Files", ACT_OPEN, &files_window },
    { "Terminal", ACT_OPEN, &terminal_window },
    { "Notepad", ACT_OPEN, &notepad_window },
    { "Task Manager", ACT_OPEN, &taskman_window },
    { "System", ACT_OPEN, &system_window },
    { "Welcome", ACT_OPEN, &welcome_window },
    { 0, ACT_SEPARATOR, 0 },
    { "Minesweeper", ACT_OPEN, &mines_window },
    { 0, ACT_SEPARATOR, 0 },
    { "Text Mode", ACT_TEXT_MODE, 0 },
    { "Restart", ACT_RESTART, 0 },
    { "Shut Down", ACT_SHUTDOWN, 0 },
};

#define MENU_COUNT ((int)(sizeof(menu_items) / sizeof(menu_items[0])))

static bool menu_open;
static int menu_hover = -1;

static int menu_height(void)
{
    int h = 6;
    for (int i = 0; i < MENU_COUNT; i++)
        h += menu_items[i].action == ACT_SEPARATOR ? SEP_H : ITEM_H;
    return h;
}

static int menu_y(void)
{
    return DESK_H - menu_height() + 2;
}

/*
 * the start menu item under (px, py), or -1 if there isn't one (or it's a
 * separator).
 */
static int menu_hit(int px, int py)
{
    int y = menu_y() + 3;
    if (!inside(px, py, 2 + 3 + BAND_W, y, MENU_W - 6 - BAND_W, menu_height() - 6))
        return -1;
    for (int i = 0; i < MENU_COUNT; i++) {
        bool sep = menu_items[i].action == ACT_SEPARATOR;
        int h = sep ? SEP_H : ITEM_H;
        if (py < y + h)
            return sep ? -1 : i;
        y += h;
    }
    return -1;
}

/*
 * a raised grey panel with a navy band down its left side, and the logo at
 * the bottom of the band.
 */
static void draw_menu(void)
{
    int x = 2, y = menu_y(), h = menu_height();

    gfx_fill(x, y, MENU_W, h, C_FACE);
    gfx_bevel(x, y, MENU_W, h, true);
    gfx_fill(x + 3, y + 3, BAND_W, h - 6, C_SELECT);
    ui_logo(x + 3 + (BAND_W - 16) / 2, y + h - 3 - 22, 16);

    int iy = y + 3;
    int ix = x + 3 + BAND_W;
    int iw = MENU_W - 6 - BAND_W;
    for (int i = 0; i < MENU_COUNT; i++) {
        if (menu_items[i].action == ACT_SEPARATOR) {
            gfx_fill(ix + 2, iy + 3, iw - 4, 1, C_SHADOW);
            gfx_fill(ix + 2, iy + 4, iw - 4, 1, C_WHITE);
            iy += SEP_H;
            continue;
        }
        bool hot = i == menu_hover;
        if (hot)
            gfx_fill(ix, iy, iw, ITEM_H, C_SELECT);
        gfx_text(ix + 12, iy + 5, menu_items[i].label, hot ? C_WHITE : C_BLACK);
        iy += ITEM_H;
    }
}

/* ---- taskbar ---- */

/*
 * the taskbar has one button for each open window, minimized or not, sharing
 * the space between the start button and the clock. they get narrower when
 * there are lots of windows open.
 */

#define START_X 2
#define START_W 62
#define TASK_X  (START_X + START_W + 6)
#define CLOCK_W 64

/*
 * dialogs belong to whatever opened them, so they don't get a button of their
 * own.
 */
static bool on_taskbar(struct window *win)
{
    return win->open && !win->modal;
}

static int task_button_w(void)
{
    int open = 0;
    for (int i = 0; i < WIN_COUNT; i++)
        open += on_taskbar(windows[i]);
    if (!open)
        return 0;
    int w = (GFX_WIDTH - CLOCK_W - 8 - TASK_X) / open - 4;
    return w > 120 ? 120 : w;
}

/*
 * a raised grey bar: the start button, a button for each window (pressed in
 * for the one with the focus), and the clock in a sunken box on the right.
 */
static void draw_taskbar(void)
{
    int y = DESK_H;
    gfx_fill(0, y, GFX_WIDTH, TASKBAR_H, C_FACE);
    gfx_fill(0, y, GFX_WIDTH, 1, C_LIGHT);
    gfx_fill(0, y + 1, GFX_WIDTH, 1, C_WHITE);

    int shift = menu_open ? 1 : 0;
    gfx_bevel(START_X, y + 4, START_W, 22, !menu_open);
    ui_logo(START_X + 4 + shift, y + 7 + shift, 16);
    ui_text_bold(START_X + 23 + shift, y + 7 + shift, "Start", C_BLACK);

    struct window *top = focused();
    int bw = task_button_w(), bx = TASK_X;
    for (int i = 0; i < WIN_COUNT; i++) {
        if (!on_taskbar(windows[i]))
            continue;
        bool pressed = windows[i] == top;
        gfx_bevel(bx, y + 4, bw, 22, !pressed);
        if (pressed)
            gfx_fill(bx + 2, y + 6, bw - 4, 18, 0xD8D8D8);
        gfx_textn(bx + 6 + pressed, y + 7 + pressed, windows[i]->title, (bw - 10) / FONT_W, C_BLACK);
        bx += bw + 4;
    }

    int hour, minute, second;
    rtc_time(&hour, &minute, &second);
    char clock[8];
    ksnprintf(clock, sizeof(clock), "%02u:%02u", hour, minute);

    int cx = GFX_WIDTH - CLOCK_W - 2;
    gfx_bevel(cx, y + 4, CLOCK_W, 22, false);
    gfx_text(cx + (CLOCK_W - 5 * FONT_W) / 2, y + 7, clock, C_BLACK);
}

static struct window *taskbar_hit(int px, int py)
{
    int bw = task_button_w(), bx = TASK_X;
    for (int i = 0; i < WIN_COUNT; i++) {
        if (!on_taskbar(windows[i]))
            continue;
        if (inside(px, py, bx, DESK_H + 4, bw, 22))
            return windows[i];
        bx += bw + 4;
    }
    return 0;
}

/* ---- drawing everything ---- */

/*
 * draws the whole desktop from the back to the front: the wallpaper, the
 * icons, the windows, the taskbar and the start menu. a full screen window
 * gets drawn last, over the taskbar, and for its first two seconds there's a
 * little note saying how to get back out.
 */
static void draw_all(void)
{
    if (wallpaper)
        gfx_blit(0, 0, wallpaper);
    else
        gfx_fill(0, 0, GFX_WIDTH, DESK_H, C_DESKTOP);
    draw_icons();

    struct window *top = focused();
    bool fullscreen = top && top->fullscreen;
    for (int i = 0; i < WIN_COUNT; i++)
        if (visible(order[i]) && !(fullscreen && order[i] == top))
            draw_window(order[i], order[i] == top);

    draw_taskbar();
    if (menu_open)
        draw_menu();

    if (fullscreen) {
        draw_window(top, true);
        if (timer_ticks() - fullscreen_since < 2 * timer_hz()) {
            const char *hint = "Full screen. Press F11 to exit.";
            int w = strlen(hint) * FONT_W + 16;
            int x = (GFX_WIDTH - w) / 2;
            gfx_fill(x, 8, w, 24, 0xFFFFE1);
            gfx_rect(x, 8, w, 24, C_BLACK);
            gfx_text(x + 8, 12, hint, C_BLACK);
        }
    }
}

/*
 * shutting down: say goodbye, then ask the emulator to turn the power off. on
 * real hardware that might not work, so the message just stays up and the cpu
 * stops.
 */
void gui_shut_down(void)
{
    gfx_unclip();
    gfx_fill(0, 0, GFX_WIDTH, GFX_HEIGHT, C_BLACK);
    const char *l1 = "CraftOS has shut down.";
    const char *l2 = "You can turn off your computer now.";
    gfx_text((GFX_WIDTH - strlen(l1) * FONT_W) / 2, 210, l1, 0xFF9020);
    gfx_text((GFX_WIDTH - strlen(l2) * FONT_W) / 2, 234, l2, 0xFF9020);
    gfx_present(GFX_WIDTH, GFX_HEIGHT);

    machine_poweroff();
    halt_forever();
}

/* ---- input ---- */

/*
 * what a held down left button is doing: moving a window, resizing one, or
 * dragging inside one (which gets passed on to the app). press_x, press_y,
 * start_w and start_h remember where a move or resize began.
 */
enum { HOLD_NONE, HOLD_MOVE, HOLD_RESIZE, HOLD_CONTENT };

static int hold;
static struct window *held;
static int hold_edges;
static int press_x, press_y, start_w, start_h;

/*
 * two clicks count as a double click when they come within half a second and
 * a few pixels of each other. the second click of a pair starts things over,
 * so a triple click doesn't turn into two double clicks.
 */
static uint32_t last_click_tick;
static int last_click_x, last_click_y;
static bool last_click_valid;

static bool is_double_click(int px, int py)
{
    uint32_t now = timer_ticks();
    int dx = px - last_click_x, dy = py - last_click_y;
    bool dbl = last_click_valid && now - last_click_tick < timer_hz() / 2 &&
               dx > -5 && dx < 5 && dy > -5 && dy < 5;

    last_click_valid = !dbl;
    last_click_tick = now;
    last_click_x = px;
    last_click_y = py;
    return dbl;
}

/*
 * the start menu asks before restarting or shutting down, and these run if
 * you say yes.
 */
static void restart_yes(void *ctx)
{
    (void)ctx;
    machine_reboot();
}

static void shut_down_yes(void *ctx)
{
    (void)ctx;
    gui_shut_down();
}

static void run_menu_item(int i)
{
    switch (menu_items[i].action) {
    case ACT_OPEN:
        gui_open(menu_items[i].win);
        break;
    case ACT_TEXT_MODE:
        gui_request_exit();
        break;
    case ACT_RESTART:
        gui_confirm(ICON_QUESTION, "Restart", "Restart CraftOS now? Anything not saved will be lost.", restart_yes, 0);
        break;
    case ACT_SHUTDOWN:
        gui_confirm(ICON_QUESTION, "Shut Down", "Shut down CraftOS now? Anything not saved will be lost.", shut_down_yes, 0);
        break;
    }
}

static void click_content(struct window *win, int px, int py, bool dbl)
{
    int cx, cy, cw, ch;
    content_rect(win, &cx, &cy, &cw, &ch);
    if (!inside(px, py, cx, cy, cw, ch))
        return;
    hold = HOLD_CONTENT;
    held = win;
    if (win->click)
        win->click(win, px - cx, py - cy, dbl);
}

/*
 * works out what a click on a window hit: the close, maximize or minimize
 * button, a resize edge, the title bar, or else the content inside.
 */
static void click_window(struct window *win, int px, int py, bool dbl)
{
    gui_open(win);
    if (win->fullscreen) {
        click_content(win, px, py, dbl);
        return;
    }

    int bx, by;
    close_box(win, &bx, &by);
    if (inside(px, py, bx, by, TB_W, TB_H)) {
        if (!win->can_close || win->can_close(win))
            gui_close(win);
        return;
    }
    if (win->resizable) {
        max_box(win, &bx, &by);
        if (inside(px, py, bx, by, TB_W, TB_H)) {
            toggle_maximize(win);
            return;
        }
    }
    min_box(win, &bx, &by);
    if (!win->modal && inside(px, py, bx, by, TB_W, TB_H)) {
        win->minimized = true;
        return;
    }

    int edges = resize_edges(win, px, py);
    if (edges) {
        hold = HOLD_RESIZE;
        held = win;
        hold_edges = edges;
        press_x = px;
        press_y = py;
        start_w = win->w;
        start_h = win->h;
        return;
    }

    if (inside(px, py, win->x + 3, win->y + 3, win->w - 6, TITLE_H)) {
        if (dbl) {
            toggle_maximize(win);
        } else if (!win->maximized) {
            hold = HOLD_MOVE;
            held = win;
            press_x = px - win->x;
            press_y = py - win->y;
        }
        return;
    }

    click_content(win, px, py, dbl);
}

/*
 * a left click goes to the first thing it hits, from the front to the back: a
 * full screen window, the start menu, the taskbar, the windows from the top
 * down, and finally the desktop icons. returns whether anything changed on
 * screen.
 */
static bool on_press(int px, int py)
{
    bool dbl = is_double_click(px, py);
    struct window *top = focused();

    /* while a dialog is up, it's the only thing you can click */
    if (top && top->modal) {
        if (!inside(px, py, top->x, top->y, top->w, top->h))
            return false;
        click_window(top, px, py, dbl);
        return true;
    }

    if (top && top->fullscreen) {
        click_window(top, px, py, dbl);
        return true;
    }

    if (menu_open) {
        int item = menu_hit(px, py);
        menu_open = false;
        if (item >= 0)
            run_menu_item(item);
        if (item >= 0 || inside(px, py, START_X, DESK_H + 4, START_W, 22))
            return true;
    }

    if (py >= DESK_H) {
        if (inside(px, py, START_X, DESK_H + 4, START_W, 22)) {
            menu_open = true;
            menu_hover = -1;
            return true;
        }

        /*
         * the focused window's button minimizes it, and any other button
         * brings that window up.
         */
        struct window *win = taskbar_hit(px, py);
        if (win && win == top)
            win->minimized = true;
        else if (win)
            gui_open(win);
        return true;
    }

    struct window *win = window_at(px, py);
    if (win) {
        click_window(win, px, py, dbl);
        return true;
    }

    for (int i = 0; i < ICON_COUNT; i++) {
        int x, y;
        icon_pos(i, &x, &y);
        if (!inside(px, py, x, y, ICON_W, ICON_H - 8))
            continue;
        selected_icon = i;
        if (dbl)
            gui_open(icons[i].win);
        return true;
    }

    selected_icon = -1;
    return true;
}

/*
 * when you move a window, enough of it stays on screen that you can always
 * grab the title bar again. resizing stops at the window's smallest size and
 * at the edges of the desktop.
 */
static bool on_move(int px, int py)
{
    if (hold == HOLD_MOVE) {
        int x = px - press_x, y = py - press_y;
        if (x < 40 - held->w) x = 40 - held->w;
        if (x > GFX_WIDTH - 40) x = GFX_WIDTH - 40;
        if (y < 0) y = 0;
        if (y > DESK_H - TITLE_H) y = DESK_H - TITLE_H;
        held->x = x;
        held->y = y;
        return true;
    }

    if (hold == HOLD_RESIZE) {
        if (hold_edges & EDGE_RIGHT) {
            int w = start_w + px - press_x;
            if (w < held->min_w) w = held->min_w;
            if (w > GFX_WIDTH - held->x) w = GFX_WIDTH - held->x;
            held->w = w;
        }
        if (hold_edges & EDGE_BOTTOM) {
            int h = start_h + py - press_y;
            if (h < held->min_h) h = held->min_h;
            if (h > DESK_H - held->y) h = DESK_H - held->y;
            held->h = h;
        }
        return true;
    }

    if (hold == HOLD_CONTENT) {
        if (!held->drag)
            return false;
        int cx, cy, cw, ch;
        content_rect(held, &cx, &cy, &cw, &ch);
        return held->drag(held, px - cx, py - cy);
    }

    struct window *win = window_at(px, py);
    gfx_set_cursor(win ? edge_cursor(resize_edges(win, px, py)) : CURSOR_ARROW);

    if (menu_open) {
        int hover = menu_hit(px, py);
        if (hover != menu_hover) {
            menu_hover = hover;
            return true;
        }
        return false;
    }

    /* tell the window under the pointer where it is */
    if (win && win->hover) {
        int cx, cy, cw, ch;
        content_rect(win, &cx, &cy, &cw, &ch);
        if (inside(px, py, cx, cy, cw, ch))
            return win->hover(win, px - cx, py - cy);
    }
    return false;
}

/* the right button only does anything inside a window that wants it. */
static bool on_right_press(int px, int py)
{
    struct window *top = focused();
    if (top && top->modal)
        return false;
    if (menu_open) {
        menu_open = false;
        return true;
    }
    struct window *win = window_at(px, py);
    if (!win)
        return false;
    gui_open(win);
    int cx, cy, cw, ch;
    content_rect(win, &cx, &cy, &cw, &ch);
    if (win->right_click && inside(px, py, cx, cy, cw, ch))
        win->right_click(win, px - cx, py - cy);
    return true;
}

/*
 * a key press. a dialog gets every key while it's up. otherwise ctrl+alt+del
 * always opens the task manager, f11 flips the focused window in and out of
 * full screen, and anything else goes to the focused window.
 */
static bool on_key(char c)
{
    struct window *win = focused();
    if (win && win->modal)
        return win->key && win->key(win, c);

    if (c == KEY_CTRL_ALT_DEL) {
        gui_open(&taskman_window);
        return true;
    }
    if (!win)
        return false;
    if (c == KEY_F11) {
        toggle_fullscreen(win);
        return true;
    }
    return win->key && win->key(win, c);
}

/* the wheel scrolls whatever's under the pointer, three rows per notch. */
static bool on_wheel(int px, int py, int notches)
{
    struct window *top = focused();
    if (top && top->modal)
        return false;
    struct window *win = window_at(px, py);
    return win && win->scroll && win->scroll(win, notches * 3);
}

static bool exit_requested;

void gui_request_exit(void)
{
    exit_requested = true;
}

/*
 * the desktop's main loop. it waits for the next interrupt (which lets other
 * tasks have a go), then looks at what's changed: keys, the mouse, apps
 * asking to be redrawn, and the clock ticking over to a new second. if
 * anything did change, the whole screen gets redrawn; if only the mouse
 * moved, just the pointer does.
 */
static void gui_run(void)
{
    int mx = 0, my = 0, last_mx = -1, last_my = -1, wheel;
    uint8_t buttons, last_buttons = 0;
    uint32_t last_second = (uint32_t)-1;
    bool dirty = true;

    while (!exit_requested) {
        char c;
        while (keyboard_poll(&c))
            dirty |= on_key(c);

        mouse_get(&mx, &my, &buttons, &wheel);
        bool moved = mx != last_mx || my != last_my;
        if (moved)
            dirty |= on_move(mx, my);
        if ((buttons & 1) && !(last_buttons & 1))
            dirty |= on_press(mx, my);
        if (!(buttons & 1) && (last_buttons & 1) && hold == HOLD_CONTENT && held->release)
            dirty |= held->release(held);
        if (!(buttons & 1))
            hold = HOLD_NONE;
        if ((buttons & 2) && !(last_buttons & 2))
            dirty |= on_right_press(mx, my);
        if (wheel)
            dirty |= on_wheel(mx, my, wheel);
        last_buttons = buttons;

        for (int i = 0; i < WIN_COUNT; i++)
            if (windows[i]->tick && windows[i]->open && !windows[i]->minimized)
                dirty |= windows[i]->tick(windows[i]);
        if (poked) {
            poked = false;
            dirty = true;
        }

        uint32_t second = timer_ticks() / timer_hz();
        if (second != last_second) {
            last_second = second;
            dirty = true;
        }

        if (dirty) {
            draw_all();
            gfx_present(mx, my);
            dirty = false;
        } else if (moved) {
            gfx_move_cursor(mx, my);
        }
        last_mx = mx;
        last_my = my;

        task_wait();
    }
}

/*
 * the desktop always opens on a fresh welcome window, and leaving it closes
 * every app, so nothing gets left behind in the task list while you're back
 * on the text screen. the terminal's shell keeps going between visits, so
 * your current folder is still there when you come back.
 */
bool gui_start(void)
{
    static bool terminal_ready;

    if (!gfx_init())
        return false;
    load_images();
    if (!terminal_ready) {
        terminal_init();
        terminal_ready = true;
    }

    task_rename(1, "desktop");
    exit_requested = false;
    menu_open = false;
    hold = HOLD_NONE;
    gui_open(&welcome_window);

    gui_run();

    for (int i = 0; i < WIN_COUNT; i++)
        if (windows[i]->open)
            gui_close(windows[i]);
    gfx_exit();
    console_clear();
    task_rename(1, "shell");
    return true;
}
