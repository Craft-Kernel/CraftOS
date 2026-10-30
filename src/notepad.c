#include <apps.h>
#include <fs.h>
#include <gfx.h>
#include <string.h>

/*
 * notepad edits a copy of a file in a buffer of its own, and only writes it
 * back when you save. a star after the name in the title bar means you've
 * made changes that haven't been saved yet.
 *
 * the toolbar has new, save and save as. save writes back to the file you
 * opened. for a brand new document, or a file that can't be written to
 * (system files and live files open read-only), it asks for a name instead.
 * names are taken to be in your home folder unless they start with / or ~.
 *
 * if you've got unsaved changes, closing the window, starting a new document
 * or opening a different file all ask you first.
 */

#define NOTE_MAX   16384
#define TOOLBAR_H  28
#define BUTTON_H   22

static char text[NOTE_MAX + 1];
static int text_len;
static bool dirty;

static int file = -1;
static uint32_t file_id;

/*
 * the view follows the caret (which is always at the end of the text) while
 * you type. scrolling up lets go of it, and scrolling back to the bottom
 * grabs it again.
 */
static struct scrollbar sb = { .grab = -1 };
static bool follow = true;

static void draw(struct window *win, int cx, int cy, int cw, int ch);
static bool key(struct window *win, char c);
static bool click(struct window *win, int x, int y, bool double_click);
static bool drag(struct window *win, int x, int y);
static bool scroll(struct window *win, int rows);
static bool can_close(struct window *win);

struct window notepad_window = {
    .title = "Notepad", .proc = "notepad", .icon = "notes", .x = 120, .y = 90, .w = 420, .h = 280,
    .min_w = 260, .min_h = 140, .resizable = true,
    .draw = draw, .key = key, .click = click, .drag = drag, .scroll = scroll,
    .can_close = can_close,
};

enum { BTN_NEW, BTN_SAVE, BTN_SAVE_AS, BTN_COUNT };

static const struct {
    const char *label;
    int x, w;
} buttons[BTN_COUNT] = {
    [BTN_NEW] = { "New", 2, 52 },
    [BTN_SAVE] = { "Save", 58, 52 },
    [BTN_SAVE_AS] = { "Save As...", 114, 86 },
};

/* ---- the document ---- */

static bool has_file(void)
{
    if (file >= 0 && !fs_alive(file, file_id)) {
        file = -1;
        dirty = true;
    }
    return file >= 0;
}

static void update_title(void)
{
    const char *name = has_file() ? fs_node(file)->name : "untitled";
    ksnprintf(notepad_window.title, sizeof(notepad_window.title), "Notepad - %s%s", name, dirty ? "*" : "");
}

static void load(int node)
{
    if (node >= 0) {
        int len = fs_read(node, text, sizeof(text));
        text_len = len < 0 ? 0 : len;
        file = node;
        file_id = fs_node(node)->id;
    } else {
        text_len = 0;
        file = -1;
    }
    dirty = false;
    follow = true;
    sb.offset = 0;
    update_title();
}

/*
 * opening a file or starting fresh with unsaved changes asks first. the node
 * waiting to be opened (-1 for a new document) gets passed along as the
 * dialog's context, so we know what to open if you say yes.
 */
static void discard_and_load(void *ctx)
{
    load((int)(intptr_t)ctx);
}

static void load_asking(int node)
{
    if (!dirty) {
        load(node);
        return;
    }
    gui_confirm(ICON_WARNING, "Notepad",
                "You have unsaved changes. If you open another document, your changes will be lost.",
                discard_and_load, (void *)(intptr_t)node);
}

void notepad_open_file(int node)
{
    gui_open(&notepad_window);
    load_asking(node);
}

/* ---- saving ---- */

static void write_to(int node)
{
    int err = fs_write(node, text, text_len, false);
    if (err < 0) {
        char msg[96];
        ksnprintf(msg, sizeof(msg), "Couldn't save %s: %s.", fs_node(node)->name, fs_error(err));
        gui_message(ICON_FAIL, "Notepad", msg);
        return;
    }
    file = node;
    file_id = fs_node(node)->id;
    dirty = false;
    update_title();
}

static void replace_yes(void *ctx)
{
    write_to((int)(intptr_t)ctx);
}

static int home_folder(void)
{
    int home = fs_resolve(FS_ROOT, FS_HOME);
    return home >= 0 ? home : FS_ROOT;
}

static void save_as_named(const char *name, void *ctx)
{
    (void)ctx;
    char msg[128];
    int n = fs_resolve(home_folder(), name);

    if (n >= 0 && fs_node(n)->is_dir) {
        ksnprintf(msg, sizeof(msg), "%s is a folder. Pick a file name instead.", name);
        gui_message(ICON_FAIL, "Notepad", msg);
    } else if (n >= 0) {
        ksnprintf(msg, sizeof(msg), "%s already exists. Do you want to replace it?", fs_node(n)->name);
        gui_confirm(ICON_QUESTION, "Save As", msg, replace_yes, (void *)(intptr_t)n);
    } else if ((n = fs_create(home_folder(), name, false)) < 0) {
        ksnprintf(msg, sizeof(msg), "Couldn't create %s: %s.", name, fs_error(n));
        gui_message(ICON_FAIL, "Notepad", msg);
    } else {
        write_to(n);
    }
}

static void save_as(void)
{
    gui_prompt("Save As", "Save the document as:", has_file() ? fs_node(file)->name : "untitled.txt",
               save_as_named, 0);
}

static void save(void)
{
    if (has_file() && fs_editable(file))
        write_to(file);
    else
        save_as();
}

/* ---- closing ---- */

static void close_anyway(void *ctx)
{
    (void)ctx;
    dirty = false;
    gui_close(&notepad_window);
}

static bool can_close(struct window *win)
{
    (void)win;
    if (!dirty)
        return true;
    gui_confirm(ICON_WARNING, "Notepad",
                "You have unsaved changes. If you close this window, your changes will be lost.",
                close_anyway, 0);
    return false;
}

/* ---- drawing ---- */

/*
 * text wraps at the edge of the window. this walks through it once, calling
 * each() for every character with the row and column it ends up on, and hands
 * back the row and column just past the end, which is where the caret goes.
 */
static void layout(int cols, void (*each)(int row, int col, char c, void *ctx), void *ctx,
                   int *end_row, int *end_col)
{
    int row = 0, col = 0;
    for (int i = 0; i < text_len; i++) {
        if (text[i] == '\n' || col == cols) {
            row++;
            col = 0;
        }
        if (text[i] == '\n')
            continue;
        if (each)
            each(row, col, text[i], ctx);
        col++;
    }
    if (col == cols) {
        row++;
        col = 0;
    }
    *end_row = row;
    *end_col = col;
}

struct draw_ctx {
    int x, y, first, rows;
};

static void draw_char(int row, int col, char c, void *p)
{
    struct draw_ctx *d = p;
    if (row >= d->first && row < d->first + d->rows)
        gfx_char(d->x + col * FONT_W, d->y + (row - d->first) * FONT_H, c, C_BLACK);
}

/*
 * draws notepad: the toolbar, then the text in a white box with the caret on
 * the end. it lays the text out once first just to count the rows, so the
 * scrollbar knows how tall everything is, then again to actually draw the
 * rows that are showing.
 */
static void draw(struct window *win, int cx, int cy, int cw, int ch)
{
    (void)win;
    has_file();
    update_title();

    for (int i = 0; i < BTN_COUNT; i++)
        ui_button(cx + buttons[i].x, cy + 2, buttons[i].w, BUTTON_H, buttons[i].label, false);

    int ty = cy + TOOLBAR_H, th = ch - TOOLBAR_H, tw = cw - SCROLLBAR_W;
    gfx_fill(cx, ty, tw, th, C_WHITE);
    gfx_bevel(cx, ty, tw, th, false);

    int cols = (tw - 8) / FONT_W, rows = (th - 8) / FONT_H;
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;

    int end_row, end_col;
    layout(cols, 0, 0, &end_row, &end_col);
    sb.total = end_row + 1;
    sb.visible = rows;
    if (follow)
        sb.offset = ui_scroll_max(&sb);
    ui_scroll_by(&sb, 0);

    struct draw_ctx d = { cx + 4, ty + 4, sb.offset, rows };
    gfx_clip(cx + 2, ty + 2, tw - 4, th - 4);
    layout(cols, draw_char, &d, &end_row, &end_col);
    if (end_row >= sb.offset && end_row < sb.offset + rows)
        gfx_fill(cx + 4 + end_col * FONT_W, ty + 5 + (end_row - sb.offset) * FONT_H, 1, FONT_H - 2, C_BLACK);
    gfx_clip(cx, cy, cw, ch);

    ui_scrollbar(cx + tw, ty, th, &sb);
}

/* ---- input ---- */

static void update_follow(void)
{
    follow = sb.offset == ui_scroll_max(&sb);
}

/*
 * typing. the scrolling keys scroll, backspace rubs out the last letter, and
 * enter or anything printable goes on the end. any change marks the document
 * as unsaved and jumps the view back down to the caret.
 */
static bool key(struct window *win, char c)
{
    (void)win;
    if (ui_scroll_key(&sb, c)) {
        update_follow();
        return true;
    }

    if (c == '\b') {
        if (text_len == 0)
            return false;
        text_len--;
    } else if ((c == '\n' || (c >= ' ' && c <= '~')) && text_len < NOTE_MAX) {
        text[text_len++] = c;
    } else {
        return false;
    }
    dirty = true;
    follow = true;
    update_title();
    return true;
}

/*
 * a click: one of the toolbar buttons (new, save or save as), or the
 * scrollbar. clicks on the text itself don't do anything, since the caret
 * always lives at the end.
 */
static bool click(struct window *win, int x, int y, bool double_click)
{
    (void)double_click;
    sb.grab = -1;

    if (y >= 2 && y < 2 + BUTTON_H) {
        for (int i = 0; i < BTN_COUNT; i++) {
            if (x < buttons[i].x || x >= buttons[i].x + buttons[i].w)
                continue;
            if (i == BTN_NEW)
                load_asking(-1);
            else if (i == BTN_SAVE)
                save();
            else
                save_as();
        }
        return true;
    }

    if (y < TOOLBAR_H || x < win->cw - SCROLLBAR_W)
        return false;
    ui_scrollbar_click(&sb, y - TOOLBAR_H, win->ch - TOOLBAR_H);
    update_follow();
    return true;
}

static bool drag(struct window *win, int x, int y)
{
    (void)x;
    if (!ui_scrollbar_drag(&sb, y - TOOLBAR_H, win->ch - TOOLBAR_H))
        return false;
    update_follow();
    return true;
}

static bool scroll(struct window *win, int rows)
{
    (void)win;
    ui_scroll_by(&sb, rows);
    update_follow();
    return true;
}
