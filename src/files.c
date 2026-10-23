#include <apps.h>
#include <fs.h>
#include <gfx.h>
#include <string.h>

/*
 * the file manager. it shows one folder at a time, as a list with the folders
 * first. there's a toolbar across the top, the current path under it, the
 * list with a scrollbar, and a status line along the bottom.
 *
 * a click selects something and a double click opens it: folders open right
 * there, pictures open in the image viewer, and other files open in notepad.
 * new folders and files get made up names for now (you can rename them with
 * mv in the terminal).
 */

#define TOOLBAR_Y 2
#define BUTTON_H  22
#define PATH_Y    28
#define PATH_H    20
#define LIST_Y    52
#define ROW_H     18
#define STATUS_H  20

static int dir = -1;
static uint32_t dir_id;
static int selected = -1;
static uint32_t selected_id;
static char status[48];
static struct scrollbar sb = { .grab = -1 };

static void draw(struct window *win, int cx, int cy, int cw, int ch);
static bool key(struct window *win, char c);
static bool click(struct window *win, int x, int y, bool double_click);
static bool drag(struct window *win, int x, int y);
static bool scroll(struct window *win, int rows);

struct window files_window = {
    .title = "Files", .proc = "files", .x = 170, .y = 70, .w = 420, .h = 320,
    .min_w = 320, .min_h = 180, .resizable = true,
    .draw = draw, .key = key, .click = click, .drag = drag, .scroll = scroll,
};

enum { BTN_UP, BTN_NEW_FOLDER, BTN_NEW_FILE, BTN_DELETE, BTN_COUNT };

static const struct {
    const char *label;
    int x, w;
} buttons[BTN_COUNT] = {
    [BTN_UP] = { "Up", 2, 44 },
    [BTN_NEW_FOLDER] = { "New Folder", 50, 96 },
    [BTN_NEW_FILE] = { "New File", 150, 80 },
    [BTN_DELETE] = { "Delete", 234, 64 },
};

static void set_dir(int n)
{
    dir = n;
    dir_id = fs_node(n)->id;
    selected = -1;
    status[0] = '\0';
    sb.offset = 0;
}

static void select_node(int n)
{
    selected = n;
    selected_id = n >= 0 ? fs_node(n)->id : 0;
}

/*
 * the folder, or whatever's selected, might have been deleted from the
 * terminal since we last looked, so check before using them.
 */
static void check_state(void)
{
    if (!fs_alive(dir, dir_id)) {
        int home = fs_resolve(FS_ROOT, FS_HOME);
        set_dir(home >= 0 ? home : FS_ROOT);
    }
    if (selected >= 0 && !fs_alive(selected, selected_id))
        selected = -1;
}

void files_open_at(int n)
{
    set_dir(n);
    gui_open(&files_window);
}

/* how tall the list is, worked out from the content height. */
static int list_h(int ch)
{
    return ch - LIST_Y - STATUS_H - 2;
}

static int list_rows(int ch)
{
    int rows = (list_h(ch) - 4) / ROW_H;
    return rows < 1 ? 1 : rows;
}

/*
 * draws the file manager: the toolbar buttons, the path box, the list of
 * what's in the folder (each with an icon, and the selected one highlighted),
 * its scrollbar, and the status line at the bottom.
 */
static void draw(struct window *win, int cx, int cy, int cw, int ch)
{
    (void)win;
    check_state();

    for (int i = 0; i < BTN_COUNT; i++)
        ui_button(cx + buttons[i].x, cy + TOOLBAR_Y, buttons[i].w, BUTTON_H, buttons[i].label, false);

    char path[128];
    fs_path(dir, path, sizeof(path));
    gfx_fill(cx, cy + PATH_Y, cw, PATH_H, C_WHITE);
    gfx_bevel(cx, cy + PATH_Y, cw, PATH_H, false);
    gfx_textn(cx + 4, cy + PATH_Y + 2, path, (cw - 8) / FONT_W, C_BLACK);

    int lw = cw - SCROLLBAR_W, lh = list_h(ch);
    gfx_fill(cx, cy + LIST_Y, lw, lh, C_WHITE);
    gfx_bevel(cx, cy + LIST_Y, lw, lh, false);

    int children[FS_MAX_NODES];
    int count = fs_list(dir, children, FS_MAX_NODES);
    int rows = list_rows(ch);
    sb.total = count;
    sb.visible = rows;
    ui_scroll_by(&sb, 0);

    gfx_clip(cx + 2, cy + LIST_Y + 2, lw - 4, lh - 4);
    for (int i = 0; i < rows && sb.offset + i < count; i++) {
        int node = children[sb.offset + i];
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
        gfx_text(cx + 26, ry + 1, n->name, text);

        if (!n->is_dir) {
            char size[16];
            ksnprintf(size, sizeof(size), "%u B", fs_size(node));
            gfx_text(cx + lw - 8 - (int)strlen(size) * FONT_W, ry + 1, size, text);
        }
    }
    gfx_clip(cx, cy, cw, ch);

    ui_scrollbar(cx + lw, cy + LIST_Y, lh, &sb);

    /*
     * the status line shows the last thing that happened, or else what's in
     * this folder
     */
    char info[64];
    if (status[0])
        strlcpy(info, status, sizeof(info));
    else
        ksnprintf(info, sizeof(info), "%d item%s%s", count, count == 1 ? "" : "s",
                  fs_node(dir)->readonly ? " (read-only folder)" : "");
    int sy = cy + ch - STATUS_H;
    gfx_bevel(cx, sy, cw, STATUS_H, false);
    gfx_textn(cx + 6, sy + 2, info, (cw - 12) / FONT_W, C_BLACK);
}

/* errors go in the status line, and in a dialog too so you can't miss them. */
static void report(const char *what, int err)
{
    ksnprintf(status, sizeof(status), "%s: %s", what, fs_error(err));
    gui_message(ICON_FAIL, "Files", status);
}

/*
 * picks the first name nobody's using yet: the plain name first, then "name
 * 2", "name 3" and so on.
 */
static void create_new(bool is_dir)
{
    const char *base = is_dir ? "New Folder" : "new";
    const char *ext = is_dir ? "" : ".txt";
    char name[FS_NAME_MAX];

    for (int i = 1; i < 100; i++) {
        if (i == 1)
            ksnprintf(name, sizeof(name), "%s%s", base, ext);
        else
            ksnprintf(name, sizeof(name), is_dir ? "%s %d%s" : "%s%d%s", base, i, ext);

        int n = fs_create(dir, name, is_dir);
        if (n >= 0) {
            select_node(n);
            ksnprintf(status, sizeof(status), "Created %s", name);
            return;
        }
        if (n != FS_ERR_EXISTS) {
            report("Can't create", n);
            return;
        }
    }
}

static void open_node(int n)
{
    if (fs_node(n)->is_dir)
        set_dir(n);
    else if (is_image_name(fs_node(n)->name))
        imgview_open(n);
    else
        notepad_open_file(n);
}

/*
 * what each toolbar button does: up goes to the parent folder, the new
 * buttons make a folder or a file, and delete gets rid of whatever's
 * selected, for good (there's no recycle bin in this release).
 */
static void press_button(int b)
{
    switch (b) {
    case BTN_UP:
        if (dir != FS_ROOT)
            set_dir(fs_node(dir)->parent);
        break;
    case BTN_NEW_FOLDER:
        create_new(true);
        break;
    case BTN_NEW_FILE:
        create_new(false);
        break;
    case BTN_DELETE:
        if (selected < 0) {
            strlcpy(status, "Select something to delete.", sizeof(status));
        } else {
            char name[FS_NAME_MAX];
            strlcpy(name, fs_node(selected)->name, sizeof(name));
            int err = fs_remove(selected);
            if (err < 0) {
                report("Can't delete", err);
            } else {
                ksnprintf(status, sizeof(status), "Deleted %s", name);
                selected = -1;
            }
        }
        break;
    }
}

/*
 * a click in the file manager: the toolbar buttons, the scrollbar, or the
 * list. clicking an entry selects it, and double clicking opens it.
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

    int children[FS_MAX_NODES];
    int count = fs_list(dir, children, FS_MAX_NODES);
    int row = (y - LIST_Y - 2) / ROW_H;
    int index = sb.offset + row;

    status[0] = '\0';
    if (y < LIST_Y + 2 || row >= list_rows(win->ch) || index >= count) {
        selected = -1;
        return true;
    }
    if (double_click && children[index] == selected)
        open_node(children[index]);
    else
        select_node(children[index]);
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
