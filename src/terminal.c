#include <apps.h>
#include <cpu.h>
#include <gfx.h>
#include <shell.h>
#include <string.h>

/*
 * the terminal: a black window running the craftos shell (shell.c), the very
 * same one that runs on the text screen. this file is only the window part.
 *
 * the scrollback is a list of lines, kept just as they were printed. they
 * only get wrapped to the window's width when they're drawn, so resizing the
 * window rewraps everything for free. output and typing both go on the end of
 * the last line, and the line you're typing is kept in input[] as well, so it
 * can be run when you press enter.
 */

#define LINE_CHARS 200
#define TERM_LINES 300

#define C_TERM_TEXT 0xC0C0C0

static char lines[TERM_LINES][LINE_CHARS + 1];
static int line_count = 1;
static int cur_len;

static char input[SHELL_LINE_MAX + 1];
static int input_len;
static struct shell_recall recall;

/*
 * the view sticks to the bottom while new output comes in. scrolling up lets
 * go of it, and scrolling back down to the end grabs it again.
 */
static struct scrollbar sb = { .grab = -1 };
static bool follow = true;

static void draw(struct window *win, int cx, int cy, int cw, int ch);
static bool key(struct window *win, char c);
static bool click(struct window *win, int x, int y, bool double_click);
static bool drag(struct window *win, int x, int y);
static bool scroll(struct window *win, int rows);

struct window terminal_window = {
    .title = "Terminal", .proc = "terminal", .icon = "term", .x = 60, .y = 36, .w = 520, .h = 320,
    .min_w = 240, .min_h = 140, .resizable = true,
    .draw = draw, .key = key, .click = click, .drag = drag, .scroll = scroll,
};

/* ---- output ---- */

/* when the scrollback is full, the oldest line drops off the top. */
static void new_line(void)
{
    if (line_count == TERM_LINES) {
        memmove(lines[0], lines[1], sizeof(lines) - sizeof(lines[0]));
        memset(lines[TERM_LINES - 1], 0, sizeof(lines[0]));
    } else {
        line_count++;
    }
    cur_len = 0;
}

static void put_char(char c)
{
    if (c == '\n') {
        new_line();
        return;
    }
    if (cur_len == LINE_CHARS)
        new_line();
    lines[line_count - 1][cur_len++] = c;
    lines[line_count - 1][cur_len] = '\0';
}

static void write_text(const char *s, int len)
{
    for (int i = 0; i < len; i++)
        put_char(s[i]);
}

static void clear_text(void)
{
    memset(lines, 0, sizeof(lines));
    line_count = 1;
    cur_len = 0;
}

static void erase_char(void)
{
    if (cur_len == 0 && line_count > 1) {
        line_count--;
        cur_len = strlen(lines[line_count - 1]);
    }
    if (cur_len > 0)
        lines[line_count - 1][--cur_len] = '\0';
}

static struct shell sh = { .write = write_text, .clear = clear_text, .in_desktop = true };

void terminal_init(void)
{
    shell_start(&sh);
    shell_prompt(&sh);
}

/* ---- the window ---- */

/*
 * how many rows on screen one scrollback line takes up at this width. the
 * last line has room for the caret too.
 */
static int rows_for(int i, int cols)
{
    if (i == line_count - 1)
        return cur_len / cols + 1;
    int len = strlen(lines[i]);
    return len ? (len + cols - 1) / cols : 1;
}

/*
 * draws the terminal. first it works out how many screen rows all the wrapped
 * lines add up to, so the scrollbar knows how big things are (and jumps to
 * the bottom if we're following the output). then it draws just the rows that
 * fit in the window, wrapping each line as it goes, with a caret at the end
 * of the last one.
 */
static void draw(struct window *win, int cx, int cy, int cw, int ch)
{
    (void)win;
    int tw = cw - SCROLLBAR_W;
    gfx_fill(cx, cy, tw, ch, C_BLACK);
    gfx_bevel(cx, cy, tw, ch, false);

    int cols = (tw - 8) / FONT_W, rows = (ch - 8) / FONT_H;
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;

    int total = 0;
    for (int i = 0; i < line_count; i++)
        total += rows_for(i, cols);
    sb.total = total;
    sb.visible = rows;
    if (follow)
        sb.offset = ui_scroll_max(&sb);
    ui_scroll_by(&sb, 0);

    int row = 0;
    for (int i = 0; i < line_count && row < sb.offset + rows; i++) {
        int n = rows_for(i, cols);
        int len = i == line_count - 1 ? cur_len : (int)strlen(lines[i]);
        for (int r = 0; r < n; r++, row++) {
            int screen_row = row - sb.offset;
            int start = r * cols;
            if (screen_row >= 0 && screen_row < rows && start < len)
                gfx_textn(cx + 4, cy + 4 + screen_row * FONT_H, lines[i] + start, cols, C_TERM_TEXT);
        }
    }

    int caret_row = total - 1 - sb.offset;
    if (caret_row >= 0 && caret_row < rows)
        gfx_fill(cx + 4 + cur_len % cols * FONT_W, cy + 4 + caret_row * FONT_H + 13, FONT_W, 2, C_TERM_TEXT);

    ui_scrollbar(cx + tw, cy, ch, &sb);
}

static void update_follow(void)
{
    follow = sb.offset == ui_scroll_max(&sb);
}

/*
 * typing in the terminal. up and down bring back earlier commands from the
 * history, page up and page down scroll, enter runs the line you typed
 * through the shell, backspace rubs out a letter, and anything printable
 * gets added to the line. while a background job (like ping) is still going,
 * typing is ignored, but scrolling still works.
 */
static bool key(struct window *win, char c)
{
    (void)win;
    if ((c == KEY_UP || c == KEY_DOWN) && !sh.busy) {
        int old = input_len;
        if (!shell_history_step(&recall, c == KEY_UP ? -1 : 1, input, &input_len))
            return false;
        while (old--)
            erase_char();
        for (int i = 0; i < input_len; i++)
            put_char(input[i]);
        follow = true;
        return true;
    }
    if (ui_scroll_key(&sb, c)) {
        update_follow();
        return true;
    }
    if (sh.busy)
        return false;

    if (c == '\n') {
        put_char('\n');
        input[input_len] = '\0';
        shell_run(&sh, input);
        input_len = 0;
        recall.back = 0;
        /* after exit, start the next session on a clean screen */
        if (!terminal_window.open)
            clear_text();
        /* a background job prints the prompt itself when it's finished */
        if (!sh.busy)
            shell_prompt(&sh);
    } else if (c == '\b') {
        if (!input_len)
            return false;
        input_len--;
        erase_char();
    } else if (c >= ' ' && c <= '~' && input_len < SHELL_LINE_MAX) {
        input[input_len++] = c;
        put_char(c);
    } else {
        return false;
    }
    follow = true;
    return true;
}

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
