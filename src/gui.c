#include <apps.h>
#include <console.h>
#include <cpu.h>
#include <font.h>
#include <fs.h>
#include <gfx.h>
#include <gui.h>
#include <image.h>
#include <io.h>
#include <string.h>
#include <task.h>
#include <trash.h>
#include <version.h>

/*
 * the craftos desktop. this is the wallpaper with its grid of app icons, the
 * overlapping windows, and the glassy taskbar along the bottom with the start
 * orb, the pinned apps, a button for each open window and a clock. the apps
 * themselves each live in their own file, and plug in here by filling out a
 * struct window.
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

/*
 * the desktop's font, loaded along with the pictures the first time the
 * desktop starts. its lines are 15 pixels tall with the baseline at 11, so
 * text gets drawn one pixel lower than asked, which lines it up with where
 * the old 16 pixel vga font used to sit. if the font files are missing,
 * everything quietly falls back to the vga font.
 */
static struct font *ui_font, *ui_font_bold;

int ui_text_fit(int x, int y, const char *s, int max_w, uint32_t color, bool bold)
{
    struct font *f = bold ? ui_font_bold : ui_font;
    if (f)
        return gfx_font_text(f, x, y + 1, s, max_w, color);

    int max_chars = max_w > 0 ? max_w / FONT_W : 1 << 30;
    gfx_textn(x, y, s, max_chars, color);
    if (bold)
        gfx_textn(x + 1, y, s, max_chars, color);
    int len = strlen(s);
    return (len < max_chars ? len : max_chars) * FONT_W;
}

int ui_text_width(const char *s, bool bold)
{
    struct font *f = bold ? ui_font_bold : ui_font;
    return f ? gfx_font_width(f, s) : (int)strlen(s) * FONT_W;
}

void ui_text(int x, int y, const char *s, uint32_t color)
{
    ui_text_fit(x, y, s, 0, color, false);
}

void ui_text_bold(int x, int y, const char *s, uint32_t color)
{
    ui_text_fit(x, y, s, 0, color, true);
}

/*
 * gradients and rounded shapes. pretty much the whole look of the desktop is
 * built out of these two things.
 *
 * a gradient is a short list of colours, each pinned to a spot from 0 (the
 * top of the shape) to 1000 (the bottom). the glossy title bars and buttons
 * use four of them, with the middle two almost on top of each other, which
 * gives that hard shiny line halfway down.
 */

static uint32_t mix(uint32_t a, uint32_t b, int t, int n)
{
    if (n <= 0)
        return a;
    uint32_t r = ((a >> 16 & 0xFF) * (n - t) + (b >> 16 & 0xFF) * t) / n;
    uint32_t g = ((a >> 8 & 0xFF) * (n - t) + (b >> 8 & 0xFF) * t) / n;
    uint32_t bl = ((a & 0xFF) * (n - t) + (b & 0xFF) * t) / n;
    return r << 16 | g << 8 | bl;
}

uint32_t ui_mix(uint32_t a, uint32_t b, int t, int n)
{
    return mix(a, b, t, n);
}

struct grad {
    int count;
    int pos[4];
    uint32_t color[4];
};

static uint32_t grad_at(const struct grad *g, int t)
{
    if (t <= g->pos[0])
        return g->color[0];
    for (int i = 1; i < g->count; i++)
        if (t <= g->pos[i])
            return mix(g->color[i - 1], g->color[i], t - g->pos[i - 1], g->pos[i] - g->pos[i - 1]);
    return g->color[g->count - 1];
}

static struct grad gloss(uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3)
{
    return (struct grad){ 4, { 0, 500, 510, 1000 }, { c0, c1, c2, c3 } };
}

/*
 * rounded corners. for a corner of radius r, corner_inset says how many
 * pixels to leave off the end of each row so the edge follows a curve, where
 * row 0 is the very top (or bottom) row. shape_inset does that for a shape
 * whose top and bottom corners can be rounded differently.
 */
static int isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return r;
}

static int corner_inset(int r, int row)
{
    if (row >= r)
        return 0;
    int dy = r - row - 1;
    return r - isqrt(r * r - dy * dy);
}

static int shape_inset(int h, int rt, int rb, int row)
{
    int top = corner_inset(rt, row), bottom = corner_inset(rb, h - 1 - row);
    return top > bottom ? top : bottom;
}

/*
 * fills a box with a gradient running top to bottom, rounding its top corners
 * by rt and its bottom corners by rb.
 */
static void shape_fill(int x, int y, int w, int h, int rt, int rb, const struct grad *g)
{
    for (int row = 0; row < h; row++) {
        int in = shape_inset(h, rt, rb, row);
        gfx_fill(x + in, y + row, w - in * 2, 1, grad_at(g, h > 1 ? row * 1000 / (h - 1) : 0));
    }
}

/* a one pixel outline around the same rounded shape. */
static void shape_outline(int x, int y, int w, int h, int rt, int rb, uint32_t color)
{
    for (int row = 0; row < h; row++) {
        int in = shape_inset(h, rt, rb, row);
        int prev = row > 0 ? shape_inset(h, rt, rb, row - 1) : in;
        int next = row < h - 1 ? shape_inset(h, rt, rb, row + 1) : in;
        if (row == 0 || row == h - 1) {
            gfx_fill(x + in, y + row, w - in * 2, 1, color);
            continue;
        }
        /*
         * on a curve, the edge jumps sideways from one row to the next, so
         * stretch each row's edge out far enough to meet its neighbour and
         * leave no gaps
         */
        int reach = prev > next ? prev : next;
        int len = reach > in ? reach - in : 1;
        gfx_fill(x + in, y + row, len, 1, color);
        gfx_fill(x + w - in - len, y + row, len, 1, color);
    }
}

/*
 * bold text with a dark copy one pixel down and to the right behind it, so
 * titles stay readable on green.
 */
static void text_shadowed(int x, int y, const char *s, int max_w, uint32_t color, uint32_t shadow)
{
    ui_text_fit(x + 1, y + 1, s, max_w, shadow, true);
    ui_text_fit(x, y, s, max_w, color, true);
}

/*
 * buttons: rounded, white fading to cream, with a dark blue edge. the default
 * button (the one that enter presses) gets an orange ring just inside the
 * edge too, so you can tell which one it is.
 */
void ui_button_ex(int x, int y, int w, int h, const char *label, bool pressed, bool is_default)
{
    struct grad up = { 3, { 0, 850, 1000, 0 }, { 0xFFFFFF, 0xECE9D8, 0xD6D0C0, 0 } };
    struct grad down = { 3, { 0, 150, 1000, 0 }, { 0xD6D0C0, 0xE2DED0, 0xF2F0E8, 0 } };
    shape_fill(x, y, w, h, 3, 3, pressed ? &down : &up);
    shape_outline(x, y, w, h, 3, 3, 0x003C74);
    if (is_default)
        shape_outline(x + 1, y + 1, w - 2, h - 2, 2, 2, 0xF8B33A);

    int tx = x + (w - ui_text_width(label, false)) / 2 + pressed;
    int ty = y + (h - FONT_H) / 2 + pressed;
    ui_text(tx, ty, label, C_BLACK);
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
 * the app icons, from the craftos app icons design, each in four sizes. a few
 * of them don't belong to an app yet, but they get loaded anyway so they're
 * ready when one turns up.
 */
static const char *const app_names[] = {
    "files", "term", "tasks", "settings", "notes",
    "photos", "mines", "trash", "solitaire",
};
#define APP_ICONS ((int)(sizeof(app_names) / sizeof(app_names[0])))
static const int app_sizes[] = { 16, 28, 32, 48 };
static struct image *app_icons[APP_ICONS][4];
static struct image *start_cube;

/*
 * loads every picture and font the desktop uses, but only once: after the
 * first time, the desktop keeps them around between visits. the status and
 * app icons are found by name and size, like /usr/share/icons/apps/files-32.
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
    ui_font = font_load("/usr/share/fonts/sans-12.cfnt");
    ui_font_bold = font_load("/usr/share/fonts/sans-bold-12.cfnt");

    char path[64];
    for (int i = 0; i < 4; i++) {
        for (int s = 0; s < 4; s++) {
            ksnprintf(path, sizeof(path), "/usr/share/icons/status/%s-%d.cimg", status_names[i], status_sizes[s]);
            status_icons[i][s] = image_load(path);
        }
    }
    for (int i = 0; i < APP_ICONS; i++) {
        for (int s = 0; s < 4; s++) {
            ksnprintf(path, sizeof(path), "/usr/share/icons/apps/%s-%d.cimg", app_names[i], app_sizes[s]);
            app_icons[i][s] = image_load(path);
        }
    }
    start_cube = image_load("/usr/share/icons/apps/start-22.cimg");
}

/*
 * draws a status icon (info, question, warning or fail). if its picture is
 * missing, it draws a plain coloured circle with a letter on it instead.
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
    struct grad g = { 1, { 0 }, { colors[icon] } };
    shape_fill(x, y, size, size, size / 2, size / 2, &g);
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

/*
 * draws an app's icon by name. "craft" is a special case: it's the craftos
 * logo rather than an app icon, for the welcome window, and since the logo
 * doesn't come in 28 pixels, that size is the 32 pixel one shrunk down.
 */
void ui_app_icon(const char *name, int x, int y, int size)
{
    if (!name)
        return;
    if (strcmp(name, "craft") == 0) {
        if (size == 28 && logo_32)
            gfx_blit_scaled(x, y, 28, 28, logo_32);
        else
            ui_logo(x, y, size);
        return;
    }
    for (int i = 0; i < APP_ICONS; i++) {
        if (strcmp(app_names[i], name) != 0)
            continue;
        for (int s = 0; s < 4; s++)
            if (app_sizes[s] == size && app_icons[i][s])
                gfx_blit(x, y, app_icons[i][s]);
        return;
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
    ui_text(cx + 62, cy + 32, "A tiny desktop on the Craft Kernel.", C_BLACK);

    static const char *const tips[] = {
        "- Drag edges to resize, F11 for full screen.",
        "- Double-click an icon to open it.",
        "- Explore / in Files or the Terminal.",
        "- Drag files into folders or the Recycle Bin.",
        "- Ctrl+Alt+Del opens the Task Manager.",
    };
    for (int i = 0; i < 5; i++)
        ui_text(cx + 12, cy + 70 + i * 20, tips[i], C_BLACK);
}

static void draw_system(struct window *win, int cx, int cy, int cw, int ch)
{
    (void)win, (void)cw, (void)ch;
    char line[64];

    ui_status_icon(ICON_INFO, cw - 32 - 10 + cx, cy + 8, 32);
    ui_text_bold(cx + 10, cy + 8, OS_NAME " " OS_VERSION, C_BLACK);
    ui_text(cx + 10, cy + 28, "Kernel:  " KERNEL_NAME, C_BLACK);

    ksnprintf(line, sizeof(line), "Memory:  %u MiB", system_mem_kib() / 1024);
    ui_text(cx + 10, cy + 46, line, C_BLACK);

    ui_text(cx + 10, cy + 64, "Display: 640 x 480, 32-bit", C_BLACK);

    uint32_t up = timer_ticks() / timer_hz();
    ksnprintf(line, sizeof(line), "Uptime:  %u:%02u:%02u", up / 3600, up / 60 % 60, up % 60);
    ui_text(cx + 10, cy + 82, line, C_BLACK);
}

static struct window welcome_window = {
    .title = "Welcome", .proc = "welcome", .icon = "craft", .x = 200, .y = 60, .w = 400, .h = 210,
    .draw = draw_welcome,
};

static struct window system_window = {
    .title = "System", .proc = "system", .icon = "settings", .x = 260, .y = 190, .w = 300, .h = 150,
    .draw = draw_system,
};

/* ---- windows ---- */

/*
 * windows[] is every window in a fixed order, and it's what the taskbar goes
 * by. order[] is how they're stacked, from the back to the front. the
 * frontmost window that's open and not minimized is the one with the focus.
 */

static struct window *const windows[] = {
    &files_window, &terminal_window, &notepad_window, &mines_window, &solitaire_window,
    &taskman_window, &recycle_window, &system_window, &welcome_window, &imgview_window, &dialog_window,
};

#define WIN_COUNT ((int)(sizeof(windows) / sizeof(windows[0])))

static struct window *order[WIN_COUNT] = {
    &dialog_window, &imgview_window, &recycle_window, &mines_window, &solitaire_window, &system_window, &taskman_window, &notepad_window, &terminal_window, &files_window,
    &welcome_window,
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
 * the window frame: a dark outline with rounded top corners, a glossy green
 * title bar (a muted grey-green when the window doesn't have the focus), and
 * a cream body for the app to draw in.
 *
 * the buttons on the title bar go from right to left: a red close button,
 * then maximize (only if the window can be resized), then minimize.
 */

#define TB_SIZE   18
#define FRAME_R   6

static void close_box(struct window *win, int *x, int *y)
{
    *x = win->x + win->w - TB_SIZE - 4;
    *y = win->y + 3;
}

static void max_box(struct window *win, int *x, int *y)
{
    *x = win->x + win->w - TB_SIZE * 2 - 6;
    *y = win->y + 3;
}

static void min_box(struct window *win, int *x, int *y)
{
    *x = win->x + win->w - TB_SIZE * (win->resizable ? 3 : 2) - (win->resizable ? 8 : 6);
    *y = win->y + 3;
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
        *y = win->y + TITLE_H + 3;
        *w = win->w - 8;
        *h = win->h - TITLE_H - 7;
    }
    win->cw = *w;
    win->ch = *h;
}

static void title_button(int x, int y, bool close)
{
    struct grad red = gloss(0xF08A7A, 0xC8321F, 0xA8200F, 0xD04A30);
    struct grad green = gloss(0x8FDCA6, 0x3FA363, 0x2E8A50, 0x46A86A);
    shape_fill(x, y, TB_SIZE, TB_SIZE, 3, 3, close ? &red : &green);
    shape_outline(x, y, TB_SIZE, TB_SIZE, 3, 3, close ? 0x6A0F08 : 0x144D2B);
}

/*
 * draws the buttons on a window's title bar, each with its symbol in white.
 * the maximize button shows two overlapping boxes while the window is
 * maximized (meaning "restore"), and one big box otherwise. dialogs only get
 * a close button.
 */
static void draw_title_buttons(struct window *win)
{
    int bx, by;

    /* close: a white x, three pixels thick */
    close_box(win, &bx, &by);
    title_button(bx, by, true);
    for (int i = 0; i < 8; i++) {
        gfx_fill(bx + 4 + i, by + 5 + i, 3, 1, C_WHITE);
        gfx_fill(bx + 11 - i, by + 5 + i, 3, 1, C_WHITE);
    }

    if (win->resizable) {
        max_box(win, &bx, &by);
        title_button(bx, by, false);
        if (win->maximized) {
            gfx_rect(bx + 7, by + 4, 7, 6, C_WHITE);
            gfx_fill(bx + 7, by + 5, 7, 1, C_WHITE);
            shape_fill(bx + 4, by + 8, 7, 6, 0, 0, &(struct grad){ 1, { 0 }, { 0x3FA363 } });
            gfx_rect(bx + 4, by + 8, 7, 6, C_WHITE);
            gfx_fill(bx + 4, by + 9, 7, 1, C_WHITE);
        } else {
            gfx_rect(bx + 4, by + 4, 10, 10, C_WHITE);
            gfx_fill(bx + 4, by + 5, 10, 1, C_WHITE);
        }
    }

    if (win->modal)
        return;
    min_box(win, &bx, &by);
    title_button(bx, by, false);
    gfx_fill(bx + 5, by + 11, 7, 2, C_WHITE);
}

/*
 * draws one window: the frame and the title bar (with its icon, title and
 * buttons), then the app's own content. the title gets cut short so it never
 * runs under the buttons. everything is clipped, first to the window and then
 * to its content area, so an app can't scribble outside its own space. a full
 * screen window skips the frame and just draws its content over the whole
 * screen.
 */
static void draw_window(struct window *win, bool active)
{
    int cx, cy, cw, ch;
    content_rect(win, &cx, &cy, &cw, &ch);

    if (!win->fullscreen) {
        int x = win->x, y = win->y, w = win->w, h = win->h;
        gfx_clip(x, y, w, h);

        struct grad title = active ? gloss(0x5FBF7A, 0x2E8B4F, 0x1F7340, 0x2A8A4C)
                                   : gloss(0xB9CFC1, 0x92B09D, 0x86A392, 0x95AF9F);
        struct grad body = { 1, { 0 }, { C_FACE } };
        shape_fill(x, y, w, TITLE_H + 1, FRAME_R, 0, &title);
        shape_fill(x, y + TITLE_H + 1, w, h - TITLE_H - 1, 0, 2, &body);
        gfx_fill(x + 1, y + TITLE_H, w - 2, 1, active ? 0x144D2B : 0x6F8A7A);
        shape_outline(x, y, w, h, FRAME_R, 2, C_INK);

        int buttons = win->modal ? 1 : win->resizable ? 3 : 2;
        int max_w = w - 16 - buttons * (TB_SIZE + 2);
        int tx = x + 8;
        if (win->icon) {
            ui_app_icon(win->icon, x + 6, y + 3, 16);
            tx += 18;
            max_w -= 18;
        }
        text_shadowed(tx, y + 3, win->title, max_w,
                      active ? C_WHITE : 0xEEF3EF, active ? 0x0D3B20 : 0x5D7466);
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
 * one, and a double click opens its window. after the apps come the files in
 * the desktop folder in your home folder, in the same grid, and those can be
 * dragged around: onto the recycle bin, into a folder, or into a files window.
 */

#define ICON_W    80
#define ICON_H    80
#define ICON_ROWS 5
#define ICON_COLS (GFX_WIDTH / ICON_W)

static const struct {
    const char *label;
    struct window *win;
    const char *icon;
} icons[] = {
    { "Files", &files_window, "files" },
    { "Terminal", &terminal_window, "term" },
    { "Notepad", &notepad_window, "notes" },
    { "Mines", &mines_window, "mines" },
    { "Solitaire", &solitaire_window, "solitaire" },
    { "Recycle Bin", &recycle_window, "trash" },
    { "Tasks", &taskman_window, "tasks" },
    { "System", &system_window, "settings" },
    { "Welcome", &welcome_window, "craft" },
};

#define ICON_COUNT ((int)(sizeof(icons) / sizeof(icons[0])))

/* there's only room for so many desktop files; the rest still show in the file manager */
#define DESK_FILES_MAX (ICON_ROWS * ICON_COLS - ICON_COUNT)

static int selected_icon = -1;

/*
 * a file being dragged: which one (or -1 for none), and what's under the
 * pointer right now. drop_ok says whether letting go there would do
 * anything, which is what the highlight and the little label go by.
 */
enum { DROP_NONE, DROP_WINDOW, DROP_TRASH, DROP_FOLDER, DROP_DESKTOP };
static int drag_node = -1;
static uint32_t drag_id;
static int drop_kind, drop_folder;
static bool drop_ok;
static struct window *drop_win;
static char drop_hint[48];      /* what a window says letting go would do */
static int pointer_x, pointer_y;

void gui_drag_hint(const char *text)
{
    strlcpy(drop_hint, text, sizeof(drop_hint));
}

/* five icons to a column, then a new column starts to the right. */
static void icon_pos(int i, int *x, int *y)
{
    *x = 8 + i / ICON_ROWS * ICON_W;
    *y = 8 + i % ICON_ROWS * ICON_H;
}

int gui_desktop_folder(void)
{
    int home = fs_resolve(FS_ROOT, FS_HOME);
    if (home < 0)
        return -1;
    int dir = fs_resolve(home, "Desktop");
    if (dir < 0)
        dir = fs_create(home, "Desktop", true);
    return dir >= 0 && fs_node(dir)->is_dir ? dir : -1;
}

/* what's in the desktop folder, folders first, as many as fit on the screen. */
static int desktop_files(int *out)
{
    int dir = gui_desktop_folder();
    return dir < 0 ? 0 : fs_list(dir, out, DESK_FILES_MAX);
}

/* the file behind desktop icon i, or -1 if it's an app (or nothing). */
static int desktop_node(int i)
{
    int files[DESK_FILES_MAX];
    int n = desktop_files(files);
    return i >= ICON_COUNT && i - ICON_COUNT < n ? files[i - ICON_COUNT] : -1;
}

/* which desktop icon is under (px, py), apps and files alike, or -1. */
static int icon_at(int px, int py)
{
    int files[DESK_FILES_MAX];
    int total = ICON_COUNT + desktop_files(files);
    for (int i = 0; i < total; i++) {
        int x, y;
        icon_pos(i, &x, &y);
        if (inside(px, py, x + 4, y, ICON_W - 8, ICON_H - 6))
            return i;
    }
    return -1;
}

/* folders look like the files app, pictures like photos, and the rest like notepad. */
static const char *file_icon(int node)
{
    struct fs_node *n = fs_node(node);
    return n->is_dir ? "files" : is_image_name(n->name) ? "photos" : "notes";
}

/* opens a file the way the file manager does on a double click. */
static void open_file(int node)
{
    if (fs_node(node)->is_dir)
        files_open_at(node);
    else if (is_image_name(fs_node(node)->name))
        imgview_open(node);
    else
        notepad_open_file(node);
}

/*
 * one desktop icon: a 48 pixel picture with its name in white underneath,
 * with a soft shadow behind the text so it reads on any wallpaper. a lit
 * icon (the selected one, or the one a dragged file is over) sits in a pale
 * glass box. long file names get cut short to fit under the icon.
 */
static void draw_icon(int x, int y, const char *icon, const char *label, bool lit)
{
    if (lit) {
        gfx_fill_alpha(x + 4, y, ICON_W - 8, ICON_H - 6, 0x40FFFFFF);
        gfx_fill_alpha(x + 4, y, ICON_W - 8, 1, 0x60FFFFFF);
    }
    ui_app_icon(icon, x + (ICON_W - 48) / 2, y + 4, 48);

    int max_w = ICON_W - 6, lw = ui_text_width(label, false);
    if (lw > max_w)
        lw = max_w;
    int tx = x + (ICON_W - lw) / 2, ty = y + 56;
    ui_text_fit(tx + 1, ty + 1, label, max_w, 0x0A2414, false);
    ui_text_fit(tx, ty, label, max_w, C_WHITE, false);
}

static void draw_icons(void)
{
    bool dragging = drag_node >= 0 && drop_ok;
    for (int i = 0; i < ICON_COUNT; i++) {
        int x, y;
        icon_pos(i, &x, &y);
        bool over_bin = dragging && drop_kind == DROP_TRASH && icons[i].win == &recycle_window;
        draw_icon(x, y, icons[i].icon, icons[i].label, i == selected_icon || over_bin);
    }

    int files[DESK_FILES_MAX];
    int n = desktop_files(files);
    for (int k = 0; k < n; k++) {
        int x, y;
        icon_pos(ICON_COUNT + k, &x, &y);
        bool over = dragging && drop_kind == DROP_FOLDER && files[k] == drop_folder;
        draw_icon(x, y, file_icon(files[k]), fs_node(files[k])->name, ICON_COUNT + k == selected_icon || over);
    }
}

/*
 * the file being dragged, drawn just below and to the right of the pointer:
 * its icon and name in a glass chip, green when letting go would do
 * something, with a line under the name saying what.
 */
static void draw_drag(void)
{
    if (drag_node < 0)
        return;
    const char *name = fs_node(drag_node)->name;
    char hint[48] = "";
    if (drop_ok && drop_kind == DROP_TRASH)
        strlcpy(hint, "Move to the Recycle Bin", sizeof(hint));
    else if (drop_ok && drop_kind == DROP_FOLDER)
        ksnprintf(hint, sizeof(hint), "Move to %s", fs_node(drop_folder)->name);
    else if (drop_ok && drop_kind == DROP_DESKTOP)
        strlcpy(hint, "Move to the desktop", sizeof(hint));
    else if (drop_ok)
        strlcpy(hint, drop_hint[0] ? drop_hint : "Move here", sizeof(hint));

    int x = pointer_x + 12, y = pointer_y + 14;
    int tw = ui_text_width(name, false), hw = ui_text_width(hint, false);
    int w = 32 + 14 + (tw > hw ? tw : hw) + 10, h = 40;
    if (x + w > GFX_WIDTH)
        x = GFX_WIDTH - w;
    if (y + h > GFX_HEIGHT)
        y = GFX_HEIGHT - h;
    gfx_fill_alpha(x, y, w, h, drop_ok ? 0xC02E8B4F : 0xB0303A34);
    gfx_fill_alpha(x, y, w, 1, 0x60FFFFFF);
    ui_app_icon(file_icon(drag_node), x + 4, y + 4, 32);
    ui_text(x + 44, y + (hint[0] ? 3 : 12), name, C_WHITE);
    if (hint[0])
        ui_text(x + 44, y + 20, hint, 0xD4F0DC);
}

/* ---- start menu ---- */

/*
 * the start menu: a cream panel with a green band down its left side and the
 * logo at its foot. it lists every app, then text mode, restart and shut down
 * under a line.
 */

#define MENU_W  190
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
    { "Minesweeper", ACT_OPEN, &mines_window },
    { "Solitaire", ACT_OPEN, &solitaire_window },
    { "Recycle Bin", ACT_OPEN, &recycle_window },
    { "Task Manager", ACT_OPEN, &taskman_window },
    { "System", ACT_OPEN, &system_window },
    { "Welcome", ACT_OPEN, &welcome_window },
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

/* the item under (px, py), or -1 for none or the line */
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
 * draws the menu: the panel and its band first, then each item with its icon,
 * and the one under the pointer lit up in green.
 */
static void draw_menu(void)
{
    int x = 2, y = menu_y(), h = menu_height();
    struct grad body = { 1, { 0 }, { C_FACE } };
    struct grad band = { 2, { 0, 1000 }, { 0x3FA865, 0x17703E } };

    shape_fill(x, y, MENU_W, h, 4, 0, &body);
    shape_fill(x + 3, y + 3, BAND_W, h - 6, 2, 0, &band);
    shape_outline(x, y, MENU_W, h, 4, 0, C_INK);
    ui_logo(x + 3 + (BAND_W - 16) / 2, y + h - 3 - 22, 16);

    int iy = y + 3;
    int ix = x + 3 + BAND_W;
    int iw = MENU_W - 6 - BAND_W;
    for (int i = 0; i < MENU_COUNT; i++) {
        if (menu_items[i].action == ACT_SEPARATOR) {
            gfx_fill(ix + 2, iy + 3, iw - 4, 1, 0xCFCAB8);
            gfx_fill(ix + 2, iy + 4, iw - 4, 1, C_WHITE);
            iy += SEP_H;
            continue;
        }
        bool hot = i == menu_hover;
        if (hot)
            gfx_fill(ix, iy, iw, ITEM_H, C_SELECT);
        if (menu_items[i].win)
            ui_app_icon(menu_items[i].win->icon, ix + 8, iy + 5, 16);
        ui_text(ix + 32, iy + 5, menu_items[i].label, hot ? C_WHITE : C_BLACK);
        iy += ITEM_H;
    }
}

/* ---- taskbar ---- */

/*
 * the taskbar, from the left: the round start orb, the pinned apps (one click
 * opens them, and a little light under one means it's running), a button for
 * each open window sharing whatever space is left, and the clock. the window
 * buttons get narrower when there are lots of windows open.
 */

#define ORB_D    34
#define ORB_X    6
#define START_W  (ORB_X + ORB_D + 4)
#define PIN_X    (START_W + 4)
#define PIN_W    40
#define PIN_H    32
#define TASK_H   30
#define CLOCK_W  64

static struct window *const pinned[] = { &files_window, &terminal_window, &system_window, &mines_window };

#define PIN_COUNT ((int)(sizeof(pinned) / sizeof(pinned[0])))
#define TASK_X    (PIN_X + PIN_COUNT * (PIN_W + 2) + 8)

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
    return w > 140 ? 140 : w;
}

/*
 * a see-through box with its corners nipped off, which is what every button
 * on the glass taskbar is made of. light is how much white goes into it, from
 * 0 to 255.
 */
static void glass_box(int x, int y, int w, int h, int light)
{
    gfx_fill_alpha(x + 1, y, w - 2, 1, (uint32_t)(light + 48) << 24 | 0xFFFFFF);
    gfx_fill_alpha(x, y + 1, w, h - 2, (uint32_t)light << 24 | 0xFFFFFF);
    gfx_fill_alpha(x + 1, y + h - 1, w - 2, 1, 0x50000000);
}

/*
 * the start orb: a green ball lit from the top left, with a shine across its
 * top half and the white craft cube in the middle. every pixel checks 16
 * points inside itself (4 by 4) to see how much of it the circle covers, so
 * the edge comes out smooth instead of jagged.
 */
static void draw_orb(int x, int y, bool pressed)
{
    struct grad ball = { 3, { 0, 450, 1000 }, { 0x9BE15D, 0x1F9A45, 0x0A5A26 } };
    int c = ORB_D * 4;                         /* the centre and radius, in eighths of a pixel */
    int hx = ORB_D * 4 - ORB_D, hy = ORB_D * 4 - ORB_D * 2;
    int reach = ORB_D * 7;

    for (int py = 0; py < ORB_D; py++) {
        for (int px = 0; px < ORB_D; px++) {
            int cover = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int dx = px * 8 + sx * 2 + 1 - c, dy = py * 8 + sy * 2 + 1 - c;
                    cover += dx * dx + dy * dy <= c * c;
                }
            if (!cover)
                continue;

            int mx = px * 8 + 4, my = py * 8 + 4;
            int lx = mx - hx, ly = my - hy;
            int t = isqrt(lx * lx + ly * ly) * 1000 / reach;
            uint32_t color = grad_at(&ball, t > 1000 ? 1000 : t);
            if (pressed)
                color = mix(color, 0x000000, 1, 4);

            /* a darker rim, about a pixel and a half wide */
            int ex = mx - c, ey = my - c;
            int d = isqrt(ex * ex + ey * ey);
            if (d > c - 12)
                color = mix(color, 0x064018, d - (c - 12), 12);
            gfx_fill_alpha(x + px, y + py, 1, 1, (uint32_t)(cover * 255 / 16) << 24 | color);
        }
    }

    /* the shine: a pale ellipse across the top, fading out as it goes down */
    int sw = ORB_D * 7 / 10, sh = ORB_D * 4 / 10;
    for (int row = 0; row < sh; row++) {
        int dy = row * 2 - sh + 1;
        int half = sw / 2 * isqrt(sh * sh - dy * dy) / sh;
        int alpha = 0x80 - row * 0x68 / sh;
        gfx_fill_alpha(x + ORB_D / 2 - half, y + 2 + row, half * 2, 1, (uint32_t)alpha << 24 | 0xFFFFFF);
    }

    if (start_cube)
        gfx_blit(x + (ORB_D - 22) / 2, y + (ORB_D - 22) / 2 + 1, start_cube);
    else
        ui_logo(x + (ORB_D - 16) / 2, y + (ORB_D - 16) / 2, 16);
}

/*
 * the taskbar is dark green glass over the wallpaper: lighter and more
 * see-through at the top, a hard line halfway down, and a faint white edge
 * along the top.
 */
static void draw_taskbar(void)
{
    int y = DESK_H, half = TASKBAR_H / 2;
    for (int row = 0; row < TASKBAR_H; row++) {
        uint32_t color, alpha;
        if (row < half) {
            color = mix(0x3C8C5A, 0x0A321E, row, half - 1);
            alpha = 191 + 26 * row / (half - 1);
        } else {
            color = mix(0x052314, 0x0F3C23, row - half, TASKBAR_H - half - 1);
            alpha = 230 - 13 * (row - half) / (TASKBAR_H - half - 1);
        }
        gfx_fill_alpha(0, y + row, GFX_WIDTH, 1, alpha << 24 | color);
    }
    gfx_fill_alpha(0, y, GFX_WIDTH, 1, 0x66FFFFFF);

    draw_orb(ORB_X, y + (TASKBAR_H - ORB_D) / 2, menu_open);

    int py = y + (TASKBAR_H - PIN_H) / 2;
    for (int i = 0; i < PIN_COUNT; i++) {
        int px = PIN_X + i * (PIN_W + 2);
        if (pinned[i]->open) {
            glass_box(px, py, PIN_W, PIN_H, 0x24);
            gfx_fill(px + PIN_W / 2 - 6, py + PIN_H - 3, 12, 2, 0x9BE15D);
        }
        ui_app_icon(pinned[i]->icon, px + (PIN_W - 28) / 2, py + (PIN_H - 28) / 2, 28);
    }
    gfx_fill_alpha(TASK_X - 5, y + 6, 1, TASKBAR_H - 12, 0x60000000);
    gfx_fill_alpha(TASK_X - 4, y + 6, 1, TASKBAR_H - 12, 0x30FFFFFF);

    struct window *top = focused();
    int bw = task_button_w(), bx = TASK_X, by = y + (TASKBAR_H - TASK_H) / 2;
    for (int i = 0; i < WIN_COUNT; i++) {
        struct window *win = windows[i];
        if (!on_taskbar(win))
            continue;
        glass_box(bx, by, bw, TASK_H, win == top ? 0x48 : 0x18);
        int tx = bx + 8;
        if (win->icon) {
            ui_app_icon(win->icon, bx + 6, by + (TASK_H - 16) / 2, 16);
            tx += 20;
        }
        ui_text_fit(tx + 1, by + 8, win->title, bx + bw - tx - 6, 0x0A2414, false);
        ui_text_fit(tx, by + 7, win->title, bx + bw - tx - 6, C_WHITE, false);
        bx += bw + 4;
    }

    int hour, minute, second;
    rtc_time(&hour, &minute, &second);
    char clock[8];
    ksnprintf(clock, sizeof(clock), "%02u:%02u", hour, minute);

    int cx = GFX_WIDTH - CLOCK_W;
    gfx_fill_alpha(cx, y + 6, 1, TASKBAR_H - 12, 0x60000000);
    gfx_fill_alpha(cx + 1, y + 6, 1, TASKBAR_H - 12, 0x30FFFFFF);
    int tx = cx + (CLOCK_W - ui_text_width(clock, true)) / 2;
    text_shadowed(tx, y + (TASKBAR_H - 15) / 2, clock, 0, C_WHITE, 0x0A2414);
}

static int pin_hit(int px, int py)
{
    for (int i = 0; i < PIN_COUNT; i++)
        if (inside(px, py, PIN_X + i * (PIN_W + 2), DESK_H + (TASKBAR_H - PIN_H) / 2, PIN_W, PIN_H))
            return i;
    return -1;
}

static struct window *taskbar_hit(int px, int py)
{
    int bw = task_button_w(), bx = TASK_X;
    for (int i = 0; i < WIN_COUNT; i++) {
        if (!on_taskbar(windows[i]))
            continue;
        if (inside(px, py, bx, DESK_H + (TASKBAR_H - TASK_H) / 2, bw, TASK_H))
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
        gfx_fill(0, 0, GFX_WIDTH, GFX_HEIGHT, C_DESKTOP);
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
            int w = ui_text_width(hint, false) + 16;
            int x = (GFX_WIDTH - w) / 2;
            gfx_fill(x, 8, w, 24, 0xFFFFE1);
            gfx_rect(x, 8, w, 24, C_BLACK);
            ui_text(x + 8, 12, hint, C_BLACK);
        }
    }
    draw_drag();
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
    ui_text_bold((GFX_WIDTH - ui_text_width(l1, true)) / 2, 210, l1, 0xFF9020);
    ui_text((GFX_WIDTH - ui_text_width(l2, false)) / 2, 234, l2, 0xFF9020);
    gfx_present(GFX_WIDTH, GFX_HEIGHT);

    machine_poweroff();
    halt_forever();
}

/* ---- input ---- */

/*
 * what a held down left button is doing: moving a window, resizing one,
 * dragging inside one (which gets passed on to the app), pressing on a
 * desktop file (which turns into dragging it once the pointer moves a few
 * pixels), or carrying a file around. press_x, press_y, start_w and start_h
 * remember where a move, resize or press began.
 */
enum { HOLD_NONE, HOLD_MOVE, HOLD_RESIZE, HOLD_CONTENT, HOLD_ICON, HOLD_FILE };

static int hold;
static struct window *held;
static int hold_edges;
static int press_x, press_y, start_w, start_h;
static int icon_press_node;

/* ---- dragging files ---- */

/* true if node is the recycle bin itself, or anything inside it. */
static bool in_bin(int node)
{
    int bin = fs_resolve(FS_ROOT, TRASH_DIR);
    for (int n = node; bin >= 0 && n != FS_ROOT; n = fs_node(n)->parent)
        if (n == bin)
            return true;
    return bin >= 0 && node == bin;
}

bool gui_move_file(int node, int folder)
{
    if (fs_node(node)->parent == folder)
        return true;
    char path[128], msg[128];
    fs_path(folder, path, sizeof(path));
    int err = fs_move(node, FS_ROOT, path);
    if (err < 0) {
        ksnprintf(msg, sizeof(msg), "Couldn't move %s into %s: %s.", fs_node(node)->name, path, fs_error(err));
        gui_message(ICON_FAIL, "Move", msg);
        return false;
    }
    return true;
}

/*
 * works out what the dragged file is over: a window that takes drops (which
 * gets asked, so it can highlight the spot), the recycle bin icon, a folder
 * on the desktop, or the empty desktop. the taskbar, title bars and windows
 * that don't take files are nothing. a window the file just left gets told,
 * so it can clear its highlight.
 */
static void find_drop(int px, int py)
{
    struct window *old = drop_win;
    drop_kind = DROP_NONE;
    drop_ok = false;
    drop_win = 0;
    drop_hint[0] = '\0';

    if (drag_node >= 0 && py < DESK_H) {
        struct window *win = window_at(px, py);
        if (win) {
            int cx, cy, cw, ch;
            content_rect(win, &cx, &cy, &cw, &ch);
            if (win->drop && inside(px, py, cx, cy, cw, ch)) {
                drop_kind = DROP_WINDOW;
                drop_win = win;
                drop_ok = win->drop(win, px - cx, py - cy, drag_node, false);
            }
        } else {
            int i = icon_at(px, py), node = desktop_node(i);
            int parent = fs_node(drag_node)->parent;
            if (i >= 0 && i < ICON_COUNT && icons[i].win == &recycle_window) {
                drop_kind = DROP_TRASH;
                drop_ok = !in_bin(drag_node);
            } else if (node >= 0 && node != drag_node && fs_node(node)->is_dir) {
                drop_kind = DROP_FOLDER;
                drop_folder = node;
                drop_ok = parent != node;
            } else {
                drop_kind = DROP_DESKTOP;
                drop_ok = parent != gui_desktop_folder();
            }
        }
    }
    if (old && old != drop_win && old->drop)
        old->drop(old, -1, -1, drag_node, false);
}

static void start_drag(int node)
{
    drag_node = node;
    drag_id = fs_node(node)->id;
    hold = HOLD_FILE;
    held = 0;
    gfx_set_cursor(CURSOR_ARROW);
    find_drop(pointer_x, pointer_y);
}

bool gui_drag_file(int node)
{
    if (hold != HOLD_CONTENT || node < 0 || fs_node(node)->readonly)
        return false;
    start_drag(node);
    return true;
}

/*
 * letting go of a dragged file (or escape, which cancels). if where it
 * landed would take it, it gets moved there or into the recycle bin, and
 * any problem shows up as a dialog. either way the window it was over gets
 * told to clear its highlight.
 */
static bool finish_drag(bool cancel)
{
    if (drag_node < 0)
        return false;
    int node = drag_node;
    if (!fs_alive(node, drag_id))
        cancel = true;
    if (!cancel)
        find_drop(pointer_x, pointer_y);
    drag_node = -1;

    if (!cancel && drop_ok) {
        if (drop_kind == DROP_WINDOW) {
            int cx, cy, cw, ch;
            content_rect(drop_win, &cx, &cy, &cw, &ch);
            drop_win->drop(drop_win, pointer_x - cx, pointer_y - cy, node, true);
        } else if (drop_kind == DROP_TRASH) {
            char name[FS_NAME_MAX], msg[128];
            strlcpy(name, fs_node(node)->name, sizeof(name));
            int err = trash_put(node);
            if (err < 0) {
                ksnprintf(msg, sizeof(msg), "Couldn't move %s to the Recycle Bin: %s.", name, fs_error(err));
                gui_message(ICON_FAIL, "Recycle Bin", msg);
            }
        } else if (drop_kind == DROP_FOLDER) {
            gui_move_file(node, drop_folder);
        } else if (drop_kind == DROP_DESKTOP) {
            int desk = gui_desktop_folder();
            if (desk >= 0)
                gui_move_file(node, desk);
        }
    }
    if (drop_win && drop_win->drop)
        drop_win->drop(drop_win, -1, -1, node, false);
    drop_win = 0;
    drop_kind = DROP_NONE;
    drop_ok = false;
    selected_icon = -1;
    return true;
}

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

static void open_start_menu(void)
{
    menu_open = true;
    menu_hover = -1;
}

static void close_start_menu(void)
{
    menu_open = false;
}

/* the highlight follows the pointer. true if it moved, so we redraw */
static bool menu_point(int item)
{
    if (item == menu_hover)
        return false;
    menu_hover = item;
    return true;
}

/* a click while the menu is open. true if it landed on the menu */
static bool start_menu_click(int px, int py)
{
    if (!inside(px, py, 2, menu_y(), MENU_W, menu_height()))
        return false;
    int item = menu_hit(px, py);
    if (item >= 0) {
        close_start_menu();
        run_menu_item(item);
    }
    return true;
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
    if (inside(px, py, bx, by, TB_SIZE, TB_SIZE)) {
        if (!win->can_close || win->can_close(win))
            gui_close(win);
        return;
    }
    if (win->resizable) {
        max_box(win, &bx, &by);
        if (inside(px, py, bx, by, TB_SIZE, TB_SIZE)) {
            toggle_maximize(win);
            return;
        }
    }
    min_box(win, &bx, &by);
    if (!win->modal && inside(px, py, bx, by, TB_SIZE, TB_SIZE)) {
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

    if (inside(px, py, win->x, win->y, win->w, TITLE_H + 1)) {
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
    pointer_x = px;
    pointer_y = py;
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
        if (start_menu_click(px, py))
            return true;
        close_start_menu();
        if (inside(px, py, 0, DESK_H, START_W, TASKBAR_H))
            return true;
    }

    if (py >= DESK_H) {
        if (inside(px, py, 0, DESK_H, START_W, TASKBAR_H)) {
            open_start_menu();
            return true;
        }

        int pin = pin_hit(px, py);
        if (pin >= 0) {
            gui_open(pinned[pin]);
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

    /*
     * an app icon opens on a double click. a file icon opens the same way,
     * and a press on one might also be the start of dragging it.
     */
    int i = icon_at(px, py);
    if (i >= 0) {
        selected_icon = i;
        int node = desktop_node(i);
        if (node < 0) {
            if (dbl)
                gui_open(icons[i].win);
        } else if (dbl) {
            open_file(node);
        } else {
            hold = HOLD_ICON;
            icon_press_node = node;
            press_x = px;
            press_y = py;
        }
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
    pointer_x = px;
    pointer_y = py;

    if (hold == HOLD_FILE) {
        if (!fs_alive(drag_node, drag_id)) {
            finish_drag(true);
            hold = HOLD_NONE;
        } else {
            find_drop(px, py);
        }
        return true;
    }

    /* a desktop file becomes a drag once the pointer has moved a few pixels */
    if (hold == HOLD_ICON) {
        int dx = px - press_x, dy = py - press_y;
        if ((dx > 4 || dx < -4 || dy > 4 || dy < -4) && !fs_node(icon_press_node)->readonly)
            start_drag(icon_press_node);
        return hold == HOLD_FILE;
    }

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

    if (menu_open)
        return menu_point(menu_hit(px, py));

    /*
     * tell the window under the pointer where it is, for things like showing
     * a link's address
     */
    if (win && win->hover) {
        int cx, cy, cw, ch;
        content_rect(win, &cx, &cy, &cw, &ch);
        if (inside(px, py, cx, cy, cw, ch))
            return win->hover(win, px - cx, py - cy);
    }
    return false;
}

/*
 * a key press. a dialog gets every key while it's up. otherwise ctrl+alt+del
 * always opens the task manager, escape closes the start menu, f11 flips the
 * focused window in and out of full screen, and anything else goes to the
 * focused window.
 */
static bool on_key(char c)
{
    if (hold == HOLD_FILE && c == 27) {
        finish_drag(true);
        hold = HOLD_NONE;
        return true;
    }

    struct window *win = focused();
    if (win && win->modal)
        return win->key && win->key(win, c);

    if (c == KEY_CTRL_ALT_DEL) {
        gui_open(&taskman_window);
        return true;
    }
    if (menu_open && c == 27) {
        close_start_menu();
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
/* the right button only does anything inside a window that wants it. */
static bool on_right_press(int px, int py)
{
    struct window *top = focused();
    if (top && top->modal)
        return false;
    if (menu_open) {
        close_start_menu();
        return true;
    }

    struct window *win = window_at(px, py);
    if (!win)
        return false;
    gui_open(win);
    int cx, cy, cw, ch;
    content_rect(win, &cx, &cy, &cw, &ch);
    if (win->right_click && px >= cx && py >= cy && px < cx + cw && py < cy + ch)
        win->right_click(win, px - cx, py - cy);
    return true;
}

static bool on_wheel(int px, int py, int notches)
{
    struct window *top = focused();
    if (top && top->modal)
        return false;
    struct window *win = window_at(px, py);
    return win && win->scroll && win->scroll(win, notches * 3);
}

static bool exit_requested;
static volatile bool poked;

void gui_poke(void)
{
    poked = true;
}

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
        if (!(buttons & 1)) {
            if (hold == HOLD_FILE)
                dirty |= finish_drag(false);
            else if (hold == HOLD_CONTENT && held && held->release)
                dirty |= held->release(held);
            hold = HOLD_NONE;
        }
        if ((buttons & 2) && !(last_buttons & 2))
            dirty |= on_right_press(mx, my);
        if (wheel)
            dirty |= on_wheel(mx, my, wheel);
        last_buttons = buttons;

        if (poked) {
            poked = false;
            dirty = true;
        }
        for (int i = 0; i < WIN_COUNT; i++)
            if (windows[i]->tick && windows[i]->open && !windows[i]->minimized)
                dirty |= windows[i]->tick(windows[i]);

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
    close_start_menu();
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
