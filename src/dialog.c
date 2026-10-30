#include <apps.h>
#include <gfx.h>
#include <string.h>

/*
 * dialogs, laid out like the craftos status icons design: a 40 pixel status
 * icon on the left, the message beside it, and the buttons along the bottom
 * right, with the default one ringed in orange.
 *
 * there are three kinds:
 *   confirm  asks yes or no. no is the safe choice, so enter, escape and the
 *            close button all mean no. only clicking yes or typing y says
 *            yes.
 *   message  just tells you something, with an ok button.
 *   prompt   asks for a line of text (like a file name) in an input box,
 *            with ok (the default) and cancel.
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
#define INPUT_Y  (PAD + 26)
#define INPUT_H  22
#define INPUT_MAX 63

enum { KIND_CONFIRM, KIND_MESSAGE, KIND_PROMPT };

static int kind, icon;
static char message[256];
static void (*yes_fn)(void *ctx);
static void (*text_fn)(const char *text, void *ctx);
static void *fn_ctx;
static char input[INPUT_MAX + 1];
static int input_len;

static void draw(struct window *win, int cx, int cy, int cw, int ch);
static bool key(struct window *win, char c);
static bool click(struct window *win, int x, int y, bool double_click);

struct window dialog_window = {
    .title = "CraftOS", .proc = "dialog", .w = DIALOG_W, .h = DIALOG_H,
    .modal = true, .draw = draw, .key = key, .click = click,
};

static void show(int k, int ic, const char *title, const char *msg)
{
    kind = k;
    icon = ic;
    strlcpy(dialog_window.title, title, sizeof(dialog_window.title));
    strlcpy(message, msg, sizeof(message));
    dialog_window.x = (GFX_WIDTH - DIALOG_W) / 2;
    dialog_window.y = (DESK_H - DIALOG_H) / 2;
    gui_open(&dialog_window);
}

void gui_confirm(int ic, const char *title, const char *msg, void (*on_yes)(void *ctx), void *ctx)
{
    yes_fn = on_yes;
    fn_ctx = ctx;
    show(KIND_CONFIRM, ic, title, msg);
}

void gui_message(int ic, const char *title, const char *msg)
{
    show(KIND_MESSAGE, ic, title, msg);
}

void gui_prompt(const char *title, const char *msg, const char *initial,
                void (*on_ok)(const char *text, void *ctx), void *ctx)
{
    text_fn = on_ok;
    fn_ctx = ctx;
    strlcpy(input, initial ? initial : "", sizeof(input));
    input_len = strlen(input);
    show(KIND_PROMPT, ICON_QUESTION, title, msg);
}

/*
 * word wrap: break between words so every line fits the width. the font is
 * proportional, so each word gets measured instead of just counting letters.
 */
static void draw_wrapped(int x, int y, int width, const char *text)
{
    int space = ui_text_width(" ", false), at = 0, row = 0;
    char word[64];
    while (*text) {
        while (*text == ' ')
            text++;
        int len = 0;
        while (text[len] && text[len] != ' ' && len < (int)sizeof(word) - 1) {
            word[len] = text[len];
            len++;
        }
        if (!len)
            break;
        word[len] = '\0';
        int w = ui_text_width(word, false);
        if (at && at + space + w > width) {
            row++;
            at = 0;
        }
        if (at)
            at += space;
        ui_text(x + at, y + row * FONT_H, word, C_BLACK);
        at += w;
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
 * a white, sunken box with the text and a caret after it. if the text is too
 * long to fit, it shows the end of it.
 */
static void draw_input(int x, int y, int w)
{
    gfx_fill(x, y, w, INPUT_H, C_WHITE);
    gfx_bevel(x, y, w, INPUT_H, false);

    const char *shown = input;
    while (*shown && ui_text_width(shown, false) > w - 12)
        shown++;
    ui_text(x + 5, y + 3, shown, C_BLACK);
    gfx_fill(x + 5 + ui_text_width(shown, false) + 1, y + 4, 1, INPUT_H - 8, C_BLACK);
}

/*
 * draws the dialog: the status icon, the wrapped message, the input box if
 * it's a prompt, and the buttons. which buttons there are, and which one is
 * the default, depends on the kind of dialog.
 */
static void draw(struct window *win, int cx, int cy, int cw, int ch)
{
    (void)win;
    ui_status_icon(icon, cx + PAD, cy + PAD, ICON_PX);
    draw_wrapped(cx + TEXT_X, cy + PAD + 4, cw - TEXT_X - PAD, message);
    if (kind == KIND_PROMPT)
        draw_input(cx + TEXT_X, cy + INPUT_Y, cw - TEXT_X - PAD);

    static const char *const labels[][2] = {
        [KIND_CONFIRM] = { "No", "Yes" },
        [KIND_MESSAGE] = { "OK", 0 },
        [KIND_PROMPT] = { "Cancel", "OK" },
    };
    for (int i = 0; i < 2; i++) {
        if (!labels[kind][i])
            continue;
        int x, y;
        button_pos(cw, ch, i, &x, &y);
        bool is_default = kind == KIND_PROMPT ? i == 1 : i == 0;
        ui_button_ex(cx + x, cy + y, BUTTON_W, BUTTON_H, labels[kind][i], false, is_default);
    }
}

/*
 * close the dialog first, then act. saying yes to killing the kernel task
 * never comes back here, and the callback might well want to open another
 * dialog of its own.
 */
static void answer(bool yes)
{
    int k = kind;
    void (*yes_cb)(void *ctx) = yes_fn;
    void (*text_cb)(const char *text, void *ctx) = text_fn;
    void *ctx = fn_ctx;
    char text[INPUT_MAX + 1];
    strlcpy(text, input, sizeof(text));

    gui_close(&dialog_window);
    if (!yes)
        return;
    if (k == KIND_CONFIRM && yes_cb)
        yes_cb(ctx);
    if (k == KIND_PROMPT && text_cb)
        text_cb(text, ctx);
}

/* button 1 (the left one) is the "yes" button: yes, or ok in a prompt. */
static bool click(struct window *win, int x, int y, bool double_click)
{
    (void)double_click;
    for (int i = 0; i < 2; i++) {
        if (kind == KIND_MESSAGE && i == 1)
            break;
        int bx, by;
        button_pos(win->cw, win->ch, i, &bx, &by);
        if (x >= bx && x < bx + BUTTON_W && y >= by && y < by + BUTTON_H) {
            answer(i == 1);
            return true;
        }
    }
    return false;
}

/*
 * keys. in a prompt, typing edits the text, enter is ok (as long as there's
 * something typed) and escape is cancel. in the other kinds, y means yes, and
 * n, escape, enter and space all mean no, since no is always the safe answer.
 */
static bool key(struct window *win, char c)
{
    (void)win;
    if (kind == KIND_PROMPT) {
        if (c == '\n') {
            answer(input_len > 0);
        } else if (c == 27) {
            answer(false);
        } else if (c == '\b') {
            if (input_len)
                input[--input_len] = '\0';
        } else if (c >= ' ' && c <= '~' && input_len < INPUT_MAX) {
            input[input_len++] = c;
            input[input_len] = '\0';
        } else {
            return false;
        }
        return true;
    }

    if (kind == KIND_CONFIRM && (c == 'y' || c == 'Y'))
        answer(true);
    else if (c == 'n' || c == 'N' || c == 27 || c == '\n' || c == ' ')
        answer(false);
    else
        return false;
    return true;
}
