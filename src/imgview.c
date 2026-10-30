#include <apps.h>
#include <fs.h>
#include <gfx.h>
#include <image.h>
#include <string.h>

/*
 * the image viewer. it shows one picture, centred on a dark background and
 * scaled to fit the window. small pictures get blown up by whole steps (2x,
 * 3x and so on) so pixel art stays nice and crisp, and big ones shrink down
 * to fit.
 *
 * decoded pictures are kept in a small cache, by file id, so opening the same
 * file again doesn't have to decode it all over.
 */

#define CACHE_SIZE 8
#define STATUS_H   20

static struct {
    uint32_t id;
    struct image *img;
} cache[CACHE_SIZE];
static int cache_next;

static struct image *shown;
static char status[64];

static void draw(struct window *win, int cx, int cy, int cw, int ch);

struct window imgview_window = {
    .title = "Image Viewer", .proc = "imgview", .icon = "photos", .x = 90, .y = 40, .w = 460, .h = 380,
    .min_w = 200, .min_h = 160, .resizable = true, .draw = draw,
};

bool is_image_name(const char *name)
{
    int len = strlen(name);
    return len > 5 && strcmp(name + len - 5, ".cimg") == 0;
}

static struct image *load_cached(int node)
{
    uint32_t id = fs_node(node)->id;
    for (int i = 0; i < CACHE_SIZE; i++)
        if (cache[i].img && cache[i].id == id)
            return cache[i].img;

    int size;
    const uint8_t *data = fs_blob(node, &size);
    struct image *img = data ? image_decode(data, size) : 0;
    if (img) {
        cache[cache_next].id = id;
        cache[cache_next].img = img;
        cache_next = (cache_next + 1) % CACHE_SIZE;
    }
    return img;
}

void imgview_open(int node)
{
    struct fs_node *f = fs_node(node);
    struct image *img = load_cached(node);
    if (!img) {
        ksnprintf(status, sizeof(status), "%s isn't an image CraftOS can open.", f->name);
        gui_message(ICON_FAIL, "Image Viewer", status);
        return;
    }
    shown = img;
    ksnprintf(status, sizeof(status), "%s  %d x %d", f->name, shown->width, shown->height);
    ksnprintf(imgview_window.title, sizeof(imgview_window.title), "Image Viewer - %s", f->name);
    gui_open(&imgview_window);
}

/*
 * draws the viewer: the picture in the middle of the dark area, at the
 * biggest whole step up that fits (no more than 8x), or shrunk to fit if it's
 * too big, keeping its shape. the status bar along the bottom has the file's
 * name and size.
 */
static void draw(struct window *win, int cx, int cy, int cw, int ch)
{
    (void)win;
    int vh = ch - STATUS_H;
    gfx_fill(cx, cy, cw, vh, 0x202020);
    gfx_bevel(cx, cy, cw, vh, false);

    if (shown) {
        int room_w = cw - 8, room_h = vh - 8;
        int w = shown->width, h = shown->height;
        if (w <= room_w && h <= room_h) {
            int k = room_w / w < room_h / h ? room_w / w : room_h / h;
            if (k > 8)
                k = 8;
            w *= k;
            h *= k;
        } else if (w * room_h > h * room_w) {
            h = h * room_w / w;
            w = room_w;
        } else {
            w = w * room_h / h;
            h = room_h;
        }
        gfx_blit_scaled(cx + (cw - w) / 2, cy + (vh - h) / 2, w, h, shown);
    }

    int sy = cy + ch - STATUS_H;
    gfx_bevel(cx, sy, cw, STATUS_H, false);
    ui_text_fit(cx + 6, sy + 2, status, cw - 12, C_BLACK, false);
}
