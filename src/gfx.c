#include <font.h>
#include <gfx.h>
#include <image.h>
#include <io.h>
#include <stddef.h>

/*
 * graphics, through the bochs vga interface (bga), which qemu, bochs and
 * virtualbox all pretend to have. it's refreshingly simple: a handful of
 * registers behind two i/o ports. tell it the resolution and colour depth,
 * flip the enable bit, and the card hands us one big flat block of memory
 * where every pixel lives. no bios calls, no mode tables.
 */

#define BGA_INDEX 0x01CE
#define BGA_DATA  0x01CF

enum { BGA_ID, BGA_XRES, BGA_YRES, BGA_BPP, BGA_ENABLE };

#define BGA_ENABLED  0x01
#define BGA_LFB      0x40
#define BGA_MIN_ID   0xB0C0

static volatile uint32_t *screen;
static uint32_t back[GFX_WIDTH * GFX_HEIGHT];
static uint8_t font[256 * FONT_H];

static void bga_write(uint16_t reg, uint16_t val)
{
    outw(BGA_INDEX, reg);
    outw(BGA_DATA, val);
}

static uint16_t bga_read(uint16_t reg)
{
    outw(BGA_INDEX, reg);
    return inw(BGA_DATA);
}

/*
 * we need to know where that block of pixel memory is, and the card says so
 * in the first address slot (bar0) of its pci settings. we only look on bus
 * 0, since that's where qemu always puts it.
 */
static uint32_t pci_read(uint8_t dev, uint8_t offset)
{
    outl(0xCF8, 0x80000000 | dev << 11 | (offset & 0xFC));
    return inl(0xCFC);
}

static uint32_t find_framebuffer(void)
{
    for (uint8_t dev = 0; dev < 32; dev++) {
        if ((pci_read(dev, 0x00) & 0xFFFF) == 0xFFFF)
            continue;
        if (pci_read(dev, 0x08) >> 24 == 0x03)
            return pci_read(dev, 0x10) & 0xFFFFFFF0;
    }
    return 0;
}

/*
 * in text mode, the vga card keeps its 8x16 font in its own memory (plane 2),
 * one letter every 32 bytes. if we point the card's graphics controller at
 * that plane before switching modes, we can copy the whole font out, which
 * gives us a full set of letters without having to bring our own. then the
 * registers go back to their text mode settings.
 */
static void save_vga_font(void)
{
    volatile uint8_t *vram = (volatile uint8_t *)0xA0000;

    outb(0x3CE, 4); outb(0x3CF, 2);
    outb(0x3CE, 5); outb(0x3CF, 0x00);
    outb(0x3CE, 6); outb(0x3CF, 0x04);

    for (int c = 0; c < 256; c++)
        for (int y = 0; y < FONT_H; y++)
            font[c * FONT_H + y] = vram[c * 32 + y];

    outb(0x3CE, 4); outb(0x3CF, 0);
    outb(0x3CE, 5); outb(0x3CF, 0x10);
    outb(0x3CE, 6); outb(0x3CF, 0x0E);
}

static bool font_saved, active;

bool gfx_active(void)
{
    return active;
}

/*
 * switches into graphics mode: checks there's a bga card at all, finds its
 * pixel memory, saves the text font the first time, then sets 640x480 with 32
 * bits per pixel and turns it on. false if there's no card to use.
 */
bool gfx_init(void)
{
    if (active)
        return true;
    if (bga_read(BGA_ID) < BGA_MIN_ID)
        return false;
    uint32_t fb = find_framebuffer();
    if (!fb)
        return false;

    if (!font_saved) {
        save_vga_font();
        font_saved = true;
    }

    bga_write(BGA_ENABLE, 0);
    bga_write(BGA_XRES, GFX_WIDTH);
    bga_write(BGA_YRES, GFX_HEIGHT);
    bga_write(BGA_BPP, 32);
    bga_write(BGA_ENABLE, BGA_ENABLED | BGA_LFB);

    screen = (volatile uint32_t *)fb;
    active = true;
    return true;
}

/*
 * going back to text mode. switching the bga off isn't enough on its own:
 * turning it on scribbled graphics settings over the card's standard vga
 * registers, and the pixel memory overlaps where the text font lives, so both
 * have to be put back by hand.
 *
 * these are the register values for the standard 80x25 text mode (the one the
 * bios starts in): the misc output register first, then the sequencer, the
 * crt controller, the graphics controller and the attribute controller, in
 * that order.
 */
static const uint8_t text_mode_regs[] = {
    0x67,
    0x03, 0x00, 0x03, 0x00, 0x02,
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00,
};

/*
 * writes all the register values back. the crt controller locks its first
 * eight registers with a bit in register 0x11, so that bit is kept clear
 * while we write them. the attribute controller uses one port for both the
 * register number and its value, and reading 0x3da tells it to expect a
 * number next.
 */
static void write_vga_regs(const uint8_t *r)
{
    outb(0x3C2, *r++);

    for (uint8_t i = 0; i < 5; i++) {
        outb(0x3C4, i);
        outb(0x3C5, *r++);
    }

    for (uint8_t i = 0; i < 25; i++) {
        uint8_t v = *r++;
        if (i == 0x03)
            v |= 0x80;
        if (i == 0x11)
            v &= ~0x80;
        outb(0x3D4, i);
        outb(0x3D5, v);
    }

    for (uint8_t i = 0; i < 9; i++) {
        outb(0x3CE, i);
        outb(0x3CF, *r++);
    }

    for (uint8_t i = 0; i < 21; i++) {
        inb(0x3DA);
        outb(0x3C0, i);
        outb(0x3C0, *r++);
    }
    inb(0x3DA);
    outb(0x3C0, 0x20);
}

/*
 * the opposite of save_vga_font: open plane 2 for writing, copy all the
 * letters back in, and close it again.
 */
static void restore_vga_font(void)
{
    volatile uint8_t *vram = (volatile uint8_t *)0xA0000;

    outb(0x3C4, 2); outb(0x3C5, 0x04);
    outb(0x3C4, 4); outb(0x3C5, 0x06);
    outb(0x3CE, 4); outb(0x3CF, 0x02);
    outb(0x3CE, 5); outb(0x3CF, 0x00);
    outb(0x3CE, 6); outb(0x3CF, 0x04);

    for (int c = 0; c < 256; c++)
        for (int y = 0; y < 32; y++)
            vram[c * 32 + y] = y < FONT_H ? font[c * FONT_H + y] : 0;

    outb(0x3C4, 2); outb(0x3C5, 0x03);
    outb(0x3C4, 4); outb(0x3C5, 0x02);
    outb(0x3CE, 4); outb(0x3CF, 0x00);
    outb(0x3CE, 5); outb(0x3CF, 0x10);
    outb(0x3CE, 6); outb(0x3CF, 0x0E);
}

void gfx_exit(void)
{
    if (!active)
        return;
    bga_write(BGA_ENABLE, 0);
    write_vga_regs(text_mode_regs);
    if (font_saved)
        restore_vga_font();
    active = false;
}

/*
 * drawing into the back buffer. everything gets clipped to the current clip
 * rectangle, which is the whole screen unless gfx_clip() made it smaller. the
 * desktop uses that to keep each window's drawing inside its own window.
 */

static int clip_x0, clip_y0, clip_x1 = GFX_WIDTH, clip_y1 = GFX_HEIGHT;

void gfx_clip(int x, int y, int w, int h)
{
    clip_x0 = x < 0 ? 0 : x;
    clip_y0 = y < 0 ? 0 : y;
    clip_x1 = x + w > GFX_WIDTH ? GFX_WIDTH : x + w;
    clip_y1 = y + h > GFX_HEIGHT ? GFX_HEIGHT : y + h;
}

void gfx_unclip(void)
{
    clip_x0 = clip_y0 = 0;
    clip_x1 = GFX_WIDTH;
    clip_y1 = GFX_HEIGHT;
}

void gfx_fill(int x, int y, int w, int h, uint32_t color)
{
    int x1 = x + w, y1 = y + h;
    if (x < clip_x0) x = clip_x0;
    if (y < clip_y0) y = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;

    for (int py = y; py < y1; py++) {
        uint32_t *row = back + py * GFX_WIDTH;
        for (int px = x; px < x1; px++)
            row[px] = color;
    }
}

static uint32_t blend(uint32_t under, uint32_t over);

/*
 * like gfx_fill, but the colour carries an alpha in its top byte, so it's
 * mixed over whatever's already there instead of covering it. that's how the
 * glassy taskbar lets the wallpaper show through.
 */
void gfx_fill_alpha(int x, int y, int w, int h, uint32_t argb)
{
    int x1 = x + w, y1 = y + h;
    if (x < clip_x0) x = clip_x0;
    if (y < clip_y0) y = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;

    for (int py = y; py < y1; py++) {
        uint32_t *row = back + py * GFX_WIDTH;
        for (int px = x; px < x1; px++)
            row[px] = blend(row[px], argb);
    }
}

void gfx_rect(int x, int y, int w, int h, uint32_t color)
{
    gfx_fill(x, y, w, 1, color);
    gfx_fill(x, y + h - 1, w, 1, color);
    gfx_fill(x, y, 1, h, color);
    gfx_fill(x + w - 1, y, 1, h, color);
}

void gfx_char(int x, int y, char c, uint32_t color)
{
    if (x >= clip_x1 || y >= clip_y1 || x + FONT_W <= clip_x0 || y + FONT_H <= clip_y0)
        return;

    const uint8_t *glyph = font + (uint8_t)c * FONT_H;
    for (int gy = 0; gy < FONT_H; gy++) {
        int py = y + gy;
        if (py < clip_y0 || py >= clip_y1)
            continue;
        for (int gx = 0; gx < FONT_W; gx++) {
            int px = x + gx;
            if (px >= clip_x0 && px < clip_x1 && glyph[gy] & (0x80 >> gx))
                back[py * GFX_WIDTH + px] = color;
        }
    }
}

void gfx_textn(int x, int y, const char *s, int max_chars, uint32_t color)
{
    for (int i = 0; s[i] && i < max_chars; i++)
        gfx_char(x + i * FONT_W, y, s[i], color);
}

void gfx_text(int x, int y, const char *s, uint32_t color)
{
    gfx_textn(x, y, s, 1 << 30, color);
}

/*
 * the classic 3d border, two pixels thick. raised has light edges on the top
 * and left and dark ones on the bottom and right, as if a light is shining
 * from the top left; sunken swaps them round.
 */
void gfx_bevel(int x, int y, int w, int h, bool raised)
{
    uint32_t outer_tl = raised ? 0xFFFFFF : 0x808080;
    uint32_t outer_br = raised ? 0x000000 : 0xFFFFFF;
    uint32_t inner_tl = raised ? 0xDFDFDF : 0x000000;
    uint32_t inner_br = raised ? 0x808080 : 0xDFDFDF;

    gfx_fill(x, y, w, 1, outer_tl);
    gfx_fill(x, y, 1, h, outer_tl);
    gfx_fill(x, y + h - 1, w, 1, outer_br);
    gfx_fill(x + w - 1, y, 1, h, outer_br);

    gfx_fill(x + 1, y + 1, w - 2, 1, inner_tl);
    gfx_fill(x + 1, y + 1, 1, h - 2, inner_tl);
    gfx_fill(x + 1, y + h - 2, w - 2, 1, inner_br);
    gfx_fill(x + w - 2, y + 1, 1, h - 2, inner_br);
}

/*
 * pictures. a solid pixel just gets copied, a completely clear one gets
 * skipped, and anything in between is mixed with the pixel underneath by how
 * see-through it is, one colour channel at a time.
 */

static uint32_t blend(uint32_t under, uint32_t over)
{
    uint32_t a = over >> 24;
    if (a == 255)
        return over & 0xFFFFFF;
    if (a == 0)
        return under;
    uint32_t r = ((over >> 16 & 0xFF) * a + (under >> 16 & 0xFF) * (255 - a)) / 255;
    uint32_t g = ((over >> 8 & 0xFF) * a + (under >> 8 & 0xFF) * (255 - a)) / 255;
    uint32_t b = ((over & 0xFF) * a + (under & 0xFF) * (255 - a)) / 255;
    return r << 16 | g << 8 | b;
}

void gfx_blit(int x, int y, const struct image *img)
{
    gfx_blit_scaled(x, y, img->width, img->height, img);
}

/*
 * for every pixel on the screen, this picks the nearest pixel in the picture,
 * so it works for shrinking and stretching alike. at the picture's own size
 * it's just an ordinary copy.
 */
void gfx_blit_scaled(int x, int y, int w, int h, const struct image *img)
{
    if (w <= 0 || h <= 0)
        return;
    int x0 = x < clip_x0 ? clip_x0 : x;
    int y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w > clip_x1 ? clip_x1 : x + w;
    int y1 = y + h > clip_y1 ? clip_y1 : y + h;

    for (int py = y0; py < y1; py++) {
        const uint32_t *src = img->pixels + (py - y) * img->height / h * img->width;
        uint32_t *dst = back + py * GFX_WIDTH;
        for (int px = x0; px < x1; px++)
            dst[px] = blend(dst[px], src[(px - x) * img->width / w]);
    }
}

/*
 * smooth text. each letter is a little grid of coverage values (0 to 255)
 * saying how much of each pixel the letter covers, and that's how much of the
 * text colour gets mixed into the pixel underneath, the same way pictures use
 * their alpha.
 */
int gfx_font_text(const struct font *f, int x, int y, const char *s, int max_w, uint32_t color)
{
    int start = x;
    color &= 0xFFFFFF;
    for (; *s; s++) {
        const struct font_glyph *g = font_glyph(f, *s);
        if (max_w > 0 && x + g->advance - start > max_w)
            break;
        const uint8_t *cov = f->coverage + g->offset;
        int gx0 = x + g->left;
        for (int row = 0; row < f->height; row++) {
            int py = y + row;
            if (py < clip_y0 || py >= clip_y1)
                continue;
            uint32_t *dst = back + py * GFX_WIDTH;
            for (int col = 0; col < g->width; col++) {
                int px = gx0 + col;
                uint8_t a = cov[row * g->width + col];
                if (a && px >= clip_x0 && px < clip_x1)
                    dst[px] = blend(dst[px], (uint32_t)a << 24 | color);
            }
        }
        x += g->advance;
    }
    return x - start;
}

int gfx_font_width(const struct font *f, const char *s)
{
    int w = 0;
    for (; *s; s++)
        w += font_glyph(f, *s)->advance;
    return w;
}

/*
 * the mouse pointers, drawn as black outlines (#) filled with white (.). they
 * go straight onto the real screen and never into the back buffer, so moving
 * the pointer just means repainting the spot it left from the back buffer,
 * and drawing it again somewhere else.
 *
 * the hot spot is the pixel that does the pointing: the tip of the arrow, or
 * the middle of a resize arrow.
 */

struct cursor {
    int w, h, hot_x, hot_y;
    const char *const *rows;
};

static const char *const arrow_rows[] = {
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#.........# ",
    "#......#####",
    "#...#..#    ",
    "#..##..#    ",
    "#.# #..#    ",
    "##   #..#   ",
    "#    #..#   ",
    "      #..#  ",
    "      ####  ",
};

static const char *const resize_h_rows[] = {
    "   #       #   ",
    "  ##       ##  ",
    " #.#########.# ",
    "#.............#",
    " #.#########.# ",
    "  ##       ##  ",
    "   #       #   ",
};

static const char *const resize_v_rows[] = {
    "   #   ",
    "  #.#  ",
    " #...# ",
    "###.###",
    "  #.#  ",
    "  #.#  ",
    "  #.#  ",
    "  #.#  ",
    "  #.#  ",
    "  #.#  ",
    "  #.#  ",
    "###.###",
    " #...# ",
    "  #.#  ",
    "   #   ",
};

static const char *const resize_d_rows[] = {
    "#####       ",
    "#...#       ",
    "#..#        ",
    "#.#.#       ",
    "## #.#      ",
    "    #.#     ",
    "     #.#    ",
    "      #.#  #",
    "       #.#.#",
    "        #..#",
    "       #...#",
    "       #####",
};

static const struct cursor cursors[] = {
    [CURSOR_ARROW]    = { 12, 19, 0, 0, arrow_rows },
    [CURSOR_RESIZE_H] = { 15, 7, 7, 3, resize_h_rows },
    [CURSOR_RESIZE_V] = { 7, 15, 3, 7, resize_v_rows },
    [CURSOR_RESIZE_D] = { 12, 12, 6, 6, resize_d_rows },
};

static int cursor_x, cursor_y, cursor_shape;
static int drawn_x, drawn_y, drawn_w, drawn_h;

static void restore_under_cursor(void)
{
    for (int y = drawn_y; y < drawn_y + drawn_h; y++)
        for (int x = drawn_x; x < drawn_x + drawn_w; x++)
            if (x >= 0 && y >= 0 && x < GFX_WIDTH && y < GFX_HEIGHT)
                screen[y * GFX_WIDTH + x] = back[y * GFX_WIDTH + x];
}

static void draw_cursor(void)
{
    const struct cursor *c = &cursors[cursor_shape];
    drawn_x = cursor_x - c->hot_x;
    drawn_y = cursor_y - c->hot_y;
    drawn_w = c->w;
    drawn_h = c->h;

    for (int cy = 0; cy < c->h; cy++) {
        for (int cx = 0; cx < c->w; cx++) {
            int x = drawn_x + cx, y = drawn_y + cy;
            char p = c->rows[cy][cx];
            if (p != ' ' && x >= 0 && y >= 0 && x < GFX_WIDTH && y < GFX_HEIGHT)
                screen[y * GFX_WIDTH + x] = p == '#' ? 0x000000 : 0xFFFFFF;
        }
    }
}

void gfx_move_cursor(int x, int y)
{
    restore_under_cursor();
    cursor_x = x;
    cursor_y = y;
    draw_cursor();
}

void gfx_set_cursor(int shape)
{
    if (shape == cursor_shape)
        return;
    restore_under_cursor();
    cursor_shape = shape;
    draw_cursor();
}

/*
 * copies the finished picture to the screen in one go. rep movsl copies four
 * bytes per step, which is far quicker than a byte by byte loop over all 1.2
 * mb of it.
 */
void gfx_present(int cx, int cy)
{
    size_t n = GFX_WIDTH * GFX_HEIGHT;
    const uint32_t *src = back;
    volatile uint32_t *dst = screen;
    __asm__ volatile("rep movsl" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");

    cursor_x = cx;
    cursor_y = cy;
    draw_cursor();
}
