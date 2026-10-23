#include <apps.h>
#include <gfx.h>
#include <string.h>

/*
 * dialogs, laid out like the craftos status icons design: a 40 pixel status
 * icon on the left, the message wrapped beside it, and the buttons along the
 * bottom right.
 *
 * there are two kinds. a confirm asks yes or no, and no is the safe choice,
 * so enter, escape and the close button all mean no. only clicking yes or
 * typing y says yes. a message just tells you something, with an ok button.
 *
 * dialogs are modal, which means the rest of the desktop waits until you
 * answer one.
 */

#define DIALOG_W 420
#define DIALOG_H 170
#define ICON_PX  40
#define PAD      16
#define TEXT_X   (PAD + ICON_PX + 14)
#define BUTTON_W 80
#define BUTTON_H 24
#define GAP      8

static int icon;
static bool asking;
static char message[256];
static void (*yes_fn)(void *ctx);
static void *yes_ctx;

static void draw(struct window *win, int cx, int cy, int cw, int ch);
static bool key(struct window *win, char c);
static bool click(struct window *win, int x, int y, bool double_click);

struct window dialog_window = {
    .title = "CraftOS", .proc = "dialog", .w = DIALOG_W, .h = DIALOG_H,
    .modal = true, .draw = draw, .key = key, .click = click,
};

static void show(int kind, const char *title, const char *msg)
{
    icon = kind;
    strlcpy(dialog_window.title, title, sizeof(dialog_window.title));
    strlcpy(message, msg, sizeof(message));
    dialog_window.x = (GFX_WIDTH - DIALOG_W) / 2;
    dialog_window.y = (DESK_H - DIALOG_H) / 2;
    gui_open(&dialog_window);
}

void gui_confirm(int kind, const char *title, const char *msg, void (*on_yes)(void *ctx), void *ctx)
{
    asking = true;
    yes_fn = on_yes;
    yes_ctx = ctx;
    show(kind, title, msg);
}

void gui_message(int kind, const char *title, const char *msg)
{
    asking = false;
    yes_fn = 0;
    show(kind, title, msg);
}

/* word wrap: break between words so every line fits the width. */
static void draw_wrapped(int x, int y, int width, const char *text)
{
    int cols = width / FONT_W, col = 0, row = 0;
    while (*text) {
        while (*text == ' ')
            text++;
        int len = 0;
        while (text[len] && text[len] != ' ')
            len++;
        if (!len)
            break;
        if (col && col + 1 + len > cols) {
            row++;
            col = 0;
        }
        if (col)
            col++;
        gfx_textn(x + col * FONT_W, y + row * FONT_H, text, len, C_BLACK);
        col += len;
        text += len;
    }
}

/* buttons count from the right edge, so 0 is the one furthest right. */
static void button_pos(int cw, int ch, int from_right, int *x, int *y)
{
    *x = cw - PAD - BUTTON_W - from_right * (BUTTON_W + GAP);
    *y = ch - PAD - BUTTON_H + 4;
}

/*
 * draws the dialog: the status icon, the wrapped message, and the buttons,
 * which are yes and no (with no as the default) for a question, or just ok
 * for a message.
 */
static void draw(struct window *win, int cx, int cy, int cw, int ch)
{
    (void)win;
    ui_status_icon(icon, cx + PAD, cy + PAD, ICON_PX);
    draw_wrapped(cx + TEXT_X, cy + PAD + 4, cw - TEXT_X - PAD, message);

    int x, y;
    if (asking) {
        button_pos(cw, ch, 1, &x, &y);
        ui_button_ex(cx + x, cy + y, BUTTON_W, BUTTON_H, "Yes", false, false);
        button_pos(cw, ch, 0, &x, &y);
        ui_button_ex(cx + x, cy + y, BUTTON_W, BUTTON_H, "No", false, true);
    } else {
        button_pos(cw, ch, 0, &x, &y);
        ui_button_ex(cx + x, cy + y, BUTTON_W, BUTTON_H, "OK", false, true);
    }
}

/*
 * close the dialog first, then act, since saying yes to killing the kernel
 * task never comes back here.
 */
static void answer(bool yes)
{
    void (*fn)(void *ctx) = yes_fn;
    void *ctx = yes_ctx;
    gui_close(&dialog_window);
    if (yes && asking && fn)
        fn(ctx);
}

static bool click(struct window *win, int x, int y, bool double_click)
{
    (void)double_click;
    int buttons = asking ? 2 : 1;
    for (int i = 0; i < buttons; i++) {
        int bx, by;
        button_pos(win->cw, win->ch, i, &bx, &by);
        if (x >= bx && x < bx + BUTTON_W && y >= by && y < by + BUTTON_H) {
            answer(asking && i == 1);
            return true;
        }
    }
    return false;
}

/*
 * keys: y means yes (if it's asking), and n, escape, enter and space all mean
 * no, since no is always the safe answer.
 */
static bool key(struct window *win, char c)
{
    (void)win;
    if (asking && (c == 'y' || c == 'Y'))
        answer(true);
    else if (c == 'n' || c == 'N' || c == 27 || c == '\n' || c == ' ')
        answer(false);
    else
        return false;
    return true;
}
