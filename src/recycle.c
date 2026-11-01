#include <apps.h>
#include <fs.h>
#include <gfx.h>
#include <string.h>
#include <trash.h>

/*
 * the recycle bin window. it lists what's in ~/.trash, along with where each
 * thing came from. restore puts the selected thing back where it was. delete
 * gets rid of it for good, and empty bin gets rid of everything, and both of
 * those ask first. files dragged onto the window go into the bin, just like
 * dropping them on the recycle bin icon on the desktop.
 */

#define TOOLBAR_Y 2
#define BUTTON_H  22
#define HEAD_Y    28
#define HEAD_H    18
#define LIST_Y    (HEAD_Y + HEAD_H)
#define ROW_H     18
#define STATUS_H  20

static int selected = -1;
static uint32_t selected_id;
static char status[64];
static struct scrollbar sb = { .grab = -1 };
static bool drop_over;      /* a file is being dragged over the window */

static void draw(struct window *win, int cx, int cy, int cw, int ch);
static bool key(struct window *win, char c);
static bool click(struct window *win, int x, int y, bool double_click);
static bool drag(struct window *win, int x, int y);
static bool scroll(struct window *win, int rows);
static bool drop(struct window *win, int x, int y, int node, bool really);

struct window recycle_window = {
    .title = "Recycle Bin", .proc = "recycle", .icon = "trash", .x = 200, .y = 80, .w = 420, .h = 280,
    .min_w = 320, .min_h = 180, .resizable = true,
    .draw = draw, .key = key, .click = click, .drag = drag, .scroll = scroll, .drop = drop,
};

enum { BTN_RESTORE, BTN_DELETE, BTN_EMPTY, BTN_COUNT };

static const struct {
    const char *label;
    int x, w;
} buttons[BTN_COUNT] = {
    [BTN_RESTORE] = { "Restore", 2, 72 },
    [BTN_DELETE] = { "Delete", 78, 64 },
    [BTN_EMPTY] = { "Empty Bin", 146, 84 },
};

static int list_h(int ch)
{
    return ch - LIST_Y - STATUS_H - 2;
}

static int list_rows(int ch)
{
    int rows = (list_h(ch) - 4) / ROW_H;
    return rows < 1 ? 1 : rows;
}

static void check_state(void)
{
    if (selected >= 0 && !fs_alive(selected, selected_id))
        selected = -1;
}

/*
 * draws the bin: the toolbar, the column headings, a row for each thing in
 * the bin (its icon, its name, and the folder it was deleted from), the
 * scrollbar and the status line.
 */
static void draw(struct window *win, int cx, int cy, int cw, int ch)
{
    (void)win;
    check_state();

    for (int i = 0; i < BTN_COUNT; i++)
        ui_button(cx + buttons[i].x, cy + TOOLBAR_Y, buttons[i].w, BUTTON_H, buttons[i].label, false);

    int lw = cw - SCROLLBAR_W, lh = list_h(ch);
    int name_w = lw / 2;

    /* the column headings */
    gfx_bevel(cx, cy + HEAD_Y, name_w, HEAD_H, true);
    gfx_bevel(cx + name_w, cy + HEAD_Y, cw - name_w, HEAD_H, true);
    ui_text(cx + 26, cy + HEAD_Y + 1, "Name", C_BLACK);
    ui_text(cx + name_w + 6, cy + HEAD_Y + 1, "Deleted from", C_BLACK);

    gfx_fill(cx, cy + LIST_Y, lw, lh, drop_over ? 0xE6F5EA : C_WHITE);
    gfx_bevel(cx, cy + LIST_Y, lw, lh, false);

    int items[FS_MAX_NODES];
    int count = trash_list(items, FS_MAX_NODES);
    int rows = list_rows(ch);
    sb.total = count;
    sb.visible = rows;
    ui_scroll_by(&sb, 0);

    gfx_clip(cx + 2, cy + LIST_Y + 2, lw - 4, lh - 4);
    if (!count)
        ui_text(cx + 26, cy + LIST_Y + 6, "The Recycle Bin is empty.", 0x707070);
    for (int i = 0; i < rows && sb.offset + i < count; i++) {
        int node = items[sb.offset + i];
        struct fs_node *n = fs_node(node);
        int ry = cy + LIST_Y + 2 + i * ROW_H;
        bool sel = node == selected;
        uint32_t text = sel ? C_WHITE : C_BLACK;

        if (sel)
            gfx_fill(cx + 22, ry, lw - 26, ROW_H, C_SELECT);
        if (n->is_dir)
            ui_folder_icon(cx + 4, ry + 2);
        else if (is_image_name(n->name))
            ui_image_icon(cx + 4, ry + 2);
        else
            ui_file_icon(cx + 4, ry + 2);
        ui_text_fit(cx + 26, ry + 1, n->name, name_w - 30, text, false);
        const char *from = trash_origin(node);
        ui_text_fit(cx + name_w + 6, ry + 1, from[0] ? from : "(unknown)", lw - name_w - 12, text, false);
    }
    gfx_clip(cx, cy, cw, ch);
    ui_scrollbar(cx + lw, cy + LIST_Y, lh, &sb);

    char info[64];
    if (status[0])
        strlcpy(info, status, sizeof(info));
    else
        ksnprintf(info, sizeof(info), "%d item%s", count, count == 1 ? "" : "s");
    int sy = cy + ch - STATUS_H;
    gfx_bevel(cx, sy, cw, STATUS_H, false);
    ui_text_fit(cx + 6, sy + 2, info, cw - 12, C_BLACK, false);
}

/*
 * AI Contribution (Bug Fix): the node and id the question is about, kept
 * together. the answer used to be checked against selected_id, which moves
 * on if you click another row while the question's up, and then "yes"
 * quietly did nothing.
 */
static int delete_node = -1;
static uint32_t delete_id;

static void delete_yes(void *ctx)
{
    int node = (int)(intptr_t)ctx;
    if (node != delete_node || !fs_alive(node, delete_id))
        return;
    char name[FS_NAME_MAX];
    strlcpy(name, fs_node(node)->name, sizeof(name));
    int err = trash_delete(node);
    if (err < 0)
        ksnprintf(status, sizeof(status), "Can't delete %s: %s", name, fs_error(err));
    else
        ksnprintf(status, sizeof(status), "Deleted %s for good", name);
    selected = -1;
}

static void empty_yes(void *ctx)
{
    (void)ctx;
    int n = trash_empty();
    ksnprintf(status, sizeof(status), "Emptied the Recycle Bin (%d item%s)", n, n == 1 ? "" : "s");
    selected = -1;
}

static void restore(int node)
{
    char name[FS_NAME_MAX];
    strlcpy(name, fs_node(node)->name, sizeof(name));
    int err = trash_restore(node);
    if (err == FS_ERR_EXISTS) {
        ksnprintf(status, sizeof(status), "Can't restore %s: something there has its name", name);
        gui_message(ICON_FAIL, "Recycle Bin", status);
    } else if (err < 0) {
        ksnprintf(status, sizeof(status), "Can't restore %s: %s", name, fs_error(err));
        gui_message(ICON_FAIL, "Recycle Bin", status);
    } else {
        ksnprintf(status, sizeof(status), "Restored %s", name);
        selected = -1;
    }
}

/*
 * what each toolbar button does. restore puts the selection back, and delete
 * and empty bin both ask before getting rid of anything, since there's no
 * getting it back after that.
 */
static void press_button(int b)
{
    int items[FS_MAX_NODES];
    int count = trash_list(items, FS_MAX_NODES);
    switch (b) {
    case BTN_RESTORE:
        if (selected < 0)
            strlcpy(status, "Select something to restore.", sizeof(status));
        else
            restore(selected);
        break;
    case BTN_DELETE:
        if (selected < 0) {
            strlcpy(status, "Select something to delete.", sizeof(status));
        } else {
            static char msg[96];
            ksnprintf(msg, sizeof(msg), "Delete \"%s\" for good? This can't be undone.", fs_node(selected)->name);
            delete_node = selected;
            delete_id = fs_node(selected)->id;
            gui_confirm(ICON_WARNING, "Recycle Bin", msg, delete_yes, (void *)(intptr_t)selected);
        }
        break;
    case BTN_EMPTY:
        if (!count)
            strlcpy(status, "The Recycle Bin is already empty.", sizeof(status));
        else
            gui_confirm(ICON_WARNING, "Recycle Bin", "Delete everything in the Recycle Bin for good? This can't be undone.",
                        empty_yes, 0);
        break;
    }
}

/*
 * a click: the toolbar, the scrollbar, or a row in the list, which selects
 * it. double clicking the selected row restores it.
 */
static bool click(struct window *win, int x, int y, bool double_click)
{
    check_state();
    sb.grab = -1;

    if (y >= TOOLBAR_Y && y < TOOLBAR_Y + BUTTON_H) {
        for (int i = 0; i < BTN_COUNT; i++)
            if (x >= buttons[i].x && x < buttons[i].x + buttons[i].w)
                press_button(i);
        return true;
    }

    int lh = list_h(win->ch);
    if (y < LIST_Y || y >= LIST_Y + lh)
        return false;
    if (x >= win->cw - SCROLLBAR_W)
        return ui_scrollbar_click(&sb, y - LIST_Y, lh);

    int items[FS_MAX_NODES];
    int count = trash_list(items, FS_MAX_NODES);
    int row = (y - LIST_Y - 2) / ROW_H;
    int index = sb.offset + row;
    status[0] = '\0';
    if (y < LIST_Y + 2 || row >= list_rows(win->ch) || index >= count) {
        selected = -1;
        return true;
    }
    /* a double click puts it back */
    if (double_click && items[index] == selected) {
        restore(selected);
        return true;
    }
    selected = items[index];
    selected_id = fs_node(selected)->id;
    return true;
}

static bool drag(struct window *win, int x, int y)
{
    (void)x;
    return ui_scrollbar_drag(&sb, y - LIST_Y, list_h(win->ch));
}

static bool scroll(struct window *win, int rows)
{
    (void)win;
    ui_scroll_by(&sb, rows);
    return true;
}

static bool key(struct window *win, char c)
{
    (void)win;
    return ui_scroll_key(&sb, c);
}

/*
 * a file dragged over the bin goes into it when it's let go, unless it's
 * already in there.
 */
static bool drop(struct window *win, int x, int y, int node, bool really)
{
    (void)win, (void)y;
    int bin = fs_resolve(FS_ROOT, TRASH_DIR);
    drop_over = x >= 0 && fs_node(node)->parent != bin && node != bin;
    if (drop_over)
        gui_drag_hint("Move to the Recycle Bin");
    if (!really || !drop_over)
        return drop_over;
    drop_over = false;

    char name[FS_NAME_MAX];
    strlcpy(name, fs_node(node)->name, sizeof(name));
    int err = trash_put(node);
    if (err < 0) {
        ksnprintf(status, sizeof(status), "Can't move %s here: %s", name, fs_error(err));
        gui_message(ICON_FAIL, "Recycle Bin", status);
    } else {
        ksnprintf(status, sizeof(status), "Moved %s to the Recycle Bin", name);
    }
    return true;
}
