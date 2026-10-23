#include <apps.h>
#include <fs.h>
#include <gfx.h>
#include <string.h>

/*
 * notepad edits one buffer of text. when it's opened on a file from the ram
 * disk, that buffer is the file itself, so every key you press is saved
 * straight away, and the terminal sees the changes at once. otherwise it
 * edits a scratch buffer that isn't saved anywhere.
 *
 * read-only and live files (like /proc/uptime) open as something to look at:
 * they get read again on every redraw, so live ones keep updating, and typing
 * does nothing. if a file gets deleted while it's open, notepad quietly goes
 * back to the scratch buffer.
 */

static char scratch[FS_FILE_MAX];
static int scratch_len;
static char view[FS_FILE_MAX];

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

struct window notepad_window = {
    .title = "Notepad", .proc = "notepad", .x = 120, .y = 110, .w = 400, .h = 250,
    .min_w = 200, .min_h = 120, .resizable = true,
    .draw = draw, .key = key, .click = click, .drag = drag, .scroll = scroll,
};

/*
 * the text to show, and whether it can be edited. for a file that can be
 * edited, the length pointer is the file's own size, so edits go straight
 * into the file.
 */
static char *contents(int **len, bool *editable)
{
    static int view_len;

    if (file >= 0 && !fs_alive(file, file_id)) {
        file = -1;
        strlcpy(notepad_window.title, "Notepad", sizeof(notepad_window.title));
    }
    if (file < 0) {
        *len = &scratch_len;
        *editable = true;
        return scratch;
    }
    if (fs_editable(file)) {
        *len = &fs_node(file)->size;
        *editable = true;
        return fs_node(file)->data;
    }
    view_len = fs_read(file, view, sizeof(view));
    *len = &view_len;
    *editable = false;
    return view;
}

void notepad_open_file(int node)
{
    file = node;
    file_id = fs_node(node)->id;
    follow = fs_editable(node);
    sb.offset = 0;
    ksnprintf(notepad_window.title, sizeof(notepad_window.title), "Notepad - %s%s",
              fs_node(node)->name, fs_editable(node) ? "" : " (read-only)");
    gui_open(&notepad_window);
}

/*
 * text wraps at the edge of the window. this walks through it once, calling
 * each() for every character with the row and column it ends up on, and hands
 * back the row and column just past the end, which is where the caret goes.
 */
static void layout(const char *text, int len, int cols,
                   void (*each)(int row, int col, char c, void *ctx), void *ctx,
                   int *end_row, int *end_col)
{
    int row = 0, col = 0;
    for (int i = 0; i < len; i++) {
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
 * draws notepad: the text in a white box (grey if it can't be edited) with
 * the caret on the end, and the scrollbar. it lays the text out once first
 * just to count the rows, so the scrollbar knows how tall everything is, then
 * again to actually draw the rows that are showing.
 */
static void draw(struct window *win, int cx, int cy, int cw, int ch)
{
    (void)win;
    int *len;
    bool editable;
    char *text = contents(&len, &editable);

    int tw = cw - SCROLLBAR_W;
    gfx_fill(cx, cy, tw, ch, editable ? C_WHITE : 0xF4F4F4);
    gfx_bevel(cx, cy, tw, ch, false);

    int cols = (tw - 8) / FONT_W, rows = (ch - 8) / FONT_H;
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;

    int end_row, end_col;
    layout(text, *len, cols, 0, 0, &end_row, &end_col);
    sb.total = end_row + 1;
    sb.visible = rows;
    if (follow)
        sb.offset = ui_scroll_max(&sb);
    ui_scroll_by(&sb, 0);

    struct draw_ctx d = { cx + 4, cy + 4, sb.offset, rows };
    layout(text, *len, cols, draw_char, &d, &end_row, &end_col);

    if (editable && end_row >= sb.offset && end_row < sb.offset + rows)
        gfx_fill(cx + 4 + end_col * FONT_W, cy + 5 + (end_row - sb.offset) * FONT_H, 1, FONT_H - 2, C_BLACK);

    ui_scrollbar(cx + tw, cy, ch, &sb);
}

static void update_follow(void)
{
    follow = sb.offset == ui_scroll_max(&sb);
}

/*
 * typing. the scrolling keys scroll, backspace rubs out the last letter, and
 * enter or anything printable goes on the end, straight into the file if it's
 * one that can be edited. the view jumps back down to the caret.
 */
static bool key(struct window *win, char c)
{
    (void)win;
    if (ui_scroll_key(&sb, c)) {
        update_follow();
        return true;
    }

    int *len;
    bool editable;
    char *text = contents(&len, &editable);
    if (!editable)
        return false;

    if (c == '\b') {
        if (*len == 0)
            return false;
        (*len)--;
    } else if ((c == '\n' || (c >= ' ' && c <= '~')) && *len < FS_FILE_MAX) {
        text[(*len)++] = c;
    } else {
        return false;
    }
    follow = true;
    return true;
}

/*
 * a click on the scrollbar. clicks on the text itself don't do anything,
 * since the caret always lives at the end.
 */
static bool click(struct window *win, int x, int y, bool double_click)
{
    (void)double_click;
    if (x < win->cw - SCROLLBAR_W)
        return false;
    ui_scrollbar_click(&sb, y, win->ch);
    update_follow();
    return true;
}

static bool drag(struct window *win, int x, int y)
{
    (void)x;
    if (!ui_scrollbar_drag(&sb, y, win->ch))
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
