#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TASKBAR_H 28
#define DESK_H    (480 - TASKBAR_H)
#define TITLE_H   18

/*
 * the classic colours: grey faces with white and dark edges, and navy for
 * title bars and anything selected.
 */
#define C_DESKTOP 0x008080
#define C_FACE    0xC0C0C0
#define C_LIGHT   0xDFDFDF
#define C_WHITE   0xFFFFFF
#define C_BLACK   0x000000
#define C_SHADOW  0x808080
#define C_SELECT  0x000080
#define C_INK     0x000000

/*
 * a window on the desktop. the desktop takes care of the frame, the title bar
 * and its buttons, and all the moving, resizing, minimizing, maximizing and
 * full screen business. the app just fills in the rest through these
 * callbacks.
 *
 * draw gets the content area in screen coordinates. click and drag get a
 * point measured from the content area's top left corner. drag keeps getting
 * called while the button stays held after a click in the content, which is
 * handy for things like scrollbar thumbs. key only goes to the focused
 * window, and scroll goes to whichever window is under the mouse wheel.
 * everything except draw can be left null, and they all return true when the
 * screen needs redrawing.
 *
 * cw and ch always hold the content area's current size, so an app can work
 * out where a click landed using the same sums it used to draw.
 *
 * every open window is a task too (see task.h). proc is the name it goes by
 * there, and pid is its task number while it's open.
 *
 * a modal window (a dialog) only gets a close button, and while it's open the
 * rest of the desktop ignores the mouse and keyboard.
 */
struct window {
    char title[40];
    const char *proc;
    int pid;
    int x, y, w, h;
    int min_w, min_h;
    bool resizable;
    bool modal;

    /*
     * the app's icon name. the classic desktop draws its own icons, so here
     * it's only a label
     */
    const char *icon;

    bool open, minimized, maximized, fullscreen;
    int normal_x, normal_y, normal_w, normal_h;
    int cw, ch;

    void (*draw)(struct window *win, int cx, int cy, int cw, int ch);
    bool (*key)(struct window *win, char c);
    bool (*click)(struct window *win, int x, int y, bool double_click);
    bool (*drag)(struct window *win, int x, int y);
    bool (*scroll)(struct window *win, int lines);

    /*
     * right_click gets right button presses in the content, just like click.
     * can_close gets asked before the close button shuts the window, and an
     * app with unsaved work can say no and ask the user what to do itself.
     * release hears about it when the left button comes back up after a press
     * in the content, hover gets told where the pointer is whenever it moves
     * over the content, and tick gets called on every trip round the
     * desktop's loop. they all return true when the screen needs redrawing.
     */
    bool (*right_click)(struct window *win, int x, int y);
    bool (*can_close)(struct window *win);
    bool (*release)(struct window *win);
    bool (*hover)(struct window *win, int x, int y);
    bool (*tick)(struct window *win);
};

/*
 * asks for a redraw. any task can call this, like a download that just
 * finished.
 */
void gui_poke(void);

/*
 * text in the vga font, with (x, y) at the top left of a 16 pixel line.
 * ui_text_fit stops before it goes past max_w pixels (0 means no limit), and
 * says how wide it drew.
 */
void ui_text(int x, int y, const char *s, uint32_t color);
int ui_text_fit(int x, int y, const char *s, int max_w, uint32_t color, bool bold);
int ui_text_width(const char *s, bool bold);


/*
 * switches to graphics and runs the desktop until something asks it to stop,
 * then goes back to text mode with a clean screen. if there's no bga graphics
 * card, it gives up straight away and returns false.
 */
bool gui_start(void);
void gui_request_exit(void);

/* opens a window if it's closed or minimized, and brings it to the front. */
void gui_open(struct window *win);
void gui_close(struct window *win);

void gui_shut_down(void) __attribute__((noreturn));

/*
 * dialogs, with one of the status icons next to the message. gui_confirm asks
 * a yes or no question. on_yes(ctx) only runs if the user picks yes, and no
 * is the default, so enter, escape and the close button all count as no.
 * gui_message just tells the user something and has an ok button.
 */
void gui_confirm(int icon, const char *title, const char *message, void (*on_yes)(void *ctx), void *ctx);
void gui_message(int icon, const char *title, const char *message);

/* little drawing pieces the apps all share. */
void ui_text_bold(int x, int y, const char *s, uint32_t color);
void ui_button(int x, int y, int w, int h, const char *label, bool pressed);
void ui_button_ex(int x, int y, int w, int h, const char *label, bool pressed, bool is_default);

/* the status icons, in 16, 32, 40 or 48 pixels. */
enum { ICON_INFO, ICON_QUESTION, ICON_WARNING, ICON_FAIL };
void ui_status_icon(int icon, int x, int y, int size);
void ui_logo(int x, int y, int size);
void ui_folder_icon(int x, int y);
void ui_file_icon(int x, int y);
void ui_image_icon(int x, int y);

/*
 * a scrollbar that goes up and down, counted in rows: total is how many rows
 * of content there are, visible is how many fit on screen, and offset is the
 * first one showing. clicking an arrow moves one row, clicking the track
 * moves a whole page, and you can drag the thumb.
 */
#define SCROLLBAR_W 16

struct scrollbar {
    int total, visible, offset;
    int grab;
};

void ui_scrollbar(int x, int y, int h, struct scrollbar *sb);
bool ui_scrollbar_click(struct scrollbar *sb, int y, int h);
bool ui_scrollbar_drag(struct scrollbar *sb, int y, int h);
void ui_scroll_by(struct scrollbar *sb, int rows);
int ui_scroll_max(const struct scrollbar *sb);

/* handles the up, down, page up and page down keys. false for anything else. */
bool ui_scroll_key(struct scrollbar *sb, char c);
