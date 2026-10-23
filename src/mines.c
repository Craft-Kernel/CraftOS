#include <apps.h>
#include <cpu.h>
#include <gfx.h>
#include <string.h>

/*
 * minesweeper. it's a grid of covered squares, and some of them are hiding
 * mines. uncovering a square shows how many of its eight neighbours are
 * mines, and a square with none around it opens up its neighbours too. right
 * click puts a flag on a square you think is a mine. double click a number
 * once all its flags are down to uncover the rest of its neighbours in one
 * go. uncover every square that isn't a mine and you win.
 *
 * the mines are only laid after your first click, and never near it, so the
 * first click is always safe. the face button starts a new game.
 */

#define CELL     16
#define MAX_COLS 16
#define MAX_ROWS 16
#define LEVEL_Y  2
#define BUTTON_H 22
#define BAR_Y    30
#define BAR_H    30
#define GRID_Y   (BAR_Y + BAR_H + 6)
#define FACE     26

enum { PLAYING, WON, LOST };

static const struct {
    const char *label;
    int cols, rows, mines, button_x, button_w;
} levels[] = {
    { "Beginner", 9, 9, 10, 2, 84 },
    { "Intermediate", 16, 16, 40, 90, 104 },
};

static int level;
static int cols, rows, mine_count;

static bool mine[MAX_COLS * MAX_ROWS];
static bool open[MAX_COLS * MAX_ROWS];
static bool flag[MAX_COLS * MAX_ROWS];
static uint8_t near[MAX_COLS * MAX_ROWS];

static int state, flags_placed, opened;
static bool mines_laid;
static int boom = -1;
static uint32_t start_tick, end_tick;
static bool timing;

static void draw(struct window *win, int cx, int cy, int cw, int ch);
static bool click(struct window *win, int x, int y, bool double_click);
static bool right_click(struct window *win, int x, int y);

struct window mines_window = {
    .title = "Minesweeper", .proc = "mines", .icon = "mines", .x = 220, .y = 60, .w = 208, .h = 247,
    .draw = draw, .click = click, .right_click = right_click,
};

/*
 * a small xorshift random number generator, seeded from the clock when the
 * mines get laid.
 */
static uint32_t rng_state = 2463534242u;

static uint32_t random(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ---- the game ---- */

static int content_w(void)
{
    int grid = cols * CELL + 12;
    return grid > 200 ? grid : 200;
}

static int grid_x(void)
{
    return (content_w() - cols * CELL) / 2;
}

/* starting a game also resizes the window to fit the board. */
static void new_game(int lvl)
{
    level = lvl;
    cols = levels[lvl].cols;
    rows = levels[lvl].rows;
    mine_count = levels[lvl].mines;
    memset(mine, 0, sizeof(mine));
    memset(open, 0, sizeof(open));
    memset(flag, 0, sizeof(flag));
    memset(near, 0, sizeof(near));
    state = PLAYING;
    flags_placed = opened = 0;
    mines_laid = timing = false;
    boom = -1;

    mines_window.w = content_w() + 8;
    mines_window.h = GRID_Y + rows * CELL + 8 + 29;
}

static bool neighbour(int i, int d, int *out)
{
    int x = i % cols + d % 3 - 1, y = i / cols + d / 3 - 1;
    if (d == 4 || x < 0 || y < 0 || x >= cols || y >= rows)
        return false;
    *out = y * cols + x;
    return true;
}

/*
 * lays the mines anywhere except the first square you clicked and the squares
 * around it.
 */
static void lay_mines(int safe)
{
    rng_state ^= timer_ticks() * 2654435761u + 1;
    int sx = safe % cols, sy = safe / cols;
    for (int placed = 0; placed < mine_count;) {
        int i = random() % (cols * rows);
        int dx = i % cols - sx, dy = i / cols - sy;
        if (mine[i] || (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1))
            continue;
        mine[i] = true;
        placed++;
    }
    for (int i = 0; i < cols * rows; i++) {
        int n;
        for (int d = 0; d < 9; d++)
            if (neighbour(i, d, &n) && mine[n])
                near[i]++;
    }
    mines_laid = true;
    timing = true;
    start_tick = timer_ticks();
}

static uint32_t elapsed(void)
{
    if (!mines_laid)
        return 0;
    uint32_t end = timing ? timer_ticks() : end_tick;
    return (end - start_tick) / timer_hz();
}

static void finish(int how)
{
    state = how;
    timing = false;
    end_tick = timer_ticks();
    char msg[96];
    if (how == WON) {
        for (int i = 0; i < cols * rows; i++)
            if (mine[i])
                flag[i] = true;
        flags_placed = mine_count;
        ksnprintf(msg, sizeof(msg), "You cleared the field in %u seconds!", elapsed());
        gui_message(ICON_INFO, "Minesweeper", msg);
    } else {
        gui_message(ICON_FAIL, "Minesweeper", "Boom! That one was a mine. Click the face to try again.");
    }
}

/*
 * uncovers a square. one with no mines around it opens its neighbours too,
 * and that can spread a long way, so it keeps a list of squares still to do
 * instead of calling itself over and over.
 */
static void uncover(int start)
{
    if (open[start] || flag[start])
        return;
    if (mine[start]) {
        open[start] = true;
        boom = start;
        finish(LOST);
        return;
    }

    static int stack[MAX_COLS * MAX_ROWS];
    int top = 0;
    stack[top++] = start;
    open[start] = true;
    opened++;
    while (top) {
        int i = stack[--top], n;
        if (near[i])
            continue;
        for (int d = 0; d < 9; d++) {
            if (neighbour(i, d, &n) && !open[n] && !flag[n] && !mine[n]) {
                open[n] = true;
                opened++;
                stack[top++] = n;
            }
        }
    }
    if (opened == cols * rows - mine_count)
        finish(WON);
}

/*
 * a number with exactly that many flags around it uncovers the rest of its
 * neighbours.
 */
static void chord(int i)
{
    int n, flagged = 0;
    for (int d = 0; d < 9; d++)
        if (neighbour(i, d, &n) && flag[n])
            flagged++;
    if (flagged != near[i])
        return;
    for (int d = 0; d < 9 && state == PLAYING; d++)
        if (neighbour(i, d, &n) && !open[n] && !flag[n])
            uncover(n);
}

/* ---- drawing ---- */

static void disc(int cx, int cy, int r, uint32_t color)
{
    for (int dy = -r; dy <= r; dy++) {
        int half = 0;
        while ((half + 1) * (half + 1) + dy * dy <= r * r)
            half++;
        gfx_fill(cx - half, cy + dy, half * 2 + 1, 1, color);
    }
}

static void draw_mine(int x, int y)
{
    int cx = x + CELL / 2, cy = y + CELL / 2;
    gfx_fill(cx - 6, cy, 13, 1, C_BLACK);
    gfx_fill(cx, cy - 6, 1, 13, C_BLACK);
    for (int i = -4; i <= 4; i += 8) {
        gfx_fill(cx + i, cy + i, 1, 1, C_BLACK);
        gfx_fill(cx + i, cy - i, 1, 1, C_BLACK);
    }
    disc(cx, cy, 4, C_BLACK);
    gfx_fill(cx - 2, cy - 2, 2, 2, C_WHITE);
}

static void draw_flag(int x, int y)
{
    gfx_fill(x + 7, y + 3, 1, 9, C_BLACK);
    gfx_fill(x + 4, y + 12, 7, 1, C_BLACK);
    gfx_fill(x + 3, y + 13, 9, 1, C_BLACK);
    for (int i = 0; i < 4; i++)
        gfx_fill(x + 7 - (4 - i), y + 3 + i, 4 - i, 1, 0xE01010);
    for (int i = 0; i < 3; i++)
        gfx_fill(x + 7 - (i + 2), y + 7 + i - 1, i + 2, 1, 0xE01010);
}

static const uint32_t number_colors[9] = {
    0, 0x1040E0, 0x108020, 0xE01010, 0x102080, 0x801010, 0x108080, 0x101010, 0x707070,
};

/*
 * draws one square. a covered one is a raised button, maybe with a flag (and
 * when you've lost, a red cross on any flag that was wrong). an uncovered one
 * is flat and shows a mine, a coloured number, or nothing at all. the mine
 * that went off gets a red background.
 */
static void draw_cell(int i, int x, int y)
{
    bool show_mine = state != PLAYING && mine[i] && !flag[i];
    if (!open[i] && !show_mine) {
        gfx_fill(x, y, CELL, CELL, C_FACE);
        gfx_bevel(x, y, CELL, CELL, true);
        if (flag[i]) {
            draw_flag(x, y);
            if (state == LOST && !mine[i]) {
                for (int k = 2; k < 14; k++) {
                    gfx_fill(x + k, y + k, 1, 1, 0xE01010);
                    gfx_fill(x + 15 - k, y + k, 1, 1, 0xE01010);
                }
            }
        }
        return;
    }

    gfx_fill(x, y, CELL, CELL, i == boom ? 0xF04030 : 0xDCD8C8);
    gfx_fill(x + CELL - 1, y, 1, CELL, 0xB4AF9C);
    gfx_fill(x, y + CELL - 1, CELL, 1, 0xB4AF9C);
    if (mine[i]) {
        draw_mine(x, y);
    } else if (near[i]) {
        char s[2] = { '0' + near[i], 0 };
        ui_text_bold(x + (CELL - ui_text_width(s, true)) / 2, y, s, number_colors[near[i]]);
    }
}

/*
 * a little smiley face, a happier one when you win, and one with crossed out
 * eyes when you lose.
 */
static void draw_face(int x, int y)
{
    ui_button(x, y, FACE, FACE, "", false);
    int cx = x + FACE / 2, cy = y + FACE / 2;
    disc(cx, cy, 9, C_BLACK);
    disc(cx, cy, 8, 0xFFD000);

    if (state == LOST) {
        for (int k = -1; k <= 1; k++) {
            gfx_fill(cx - 4 + k, cy - 3 + k, 1, 1, C_BLACK);
            gfx_fill(cx - 4 + k, cy - 3 - k, 1, 1, C_BLACK);
            gfx_fill(cx + 3 + k, cy - 3 + k, 1, 1, C_BLACK);
            gfx_fill(cx + 3 + k, cy - 3 - k, 1, 1, C_BLACK);
        }
        gfx_fill(cx - 3, cy + 4, 7, 1, C_BLACK);
        gfx_fill(cx - 4, cy + 5, 1, 1, C_BLACK);
        gfx_fill(cx + 4, cy + 5, 1, 1, C_BLACK);
        return;
    }

    if (state == WON) {
        gfx_fill(cx - 6, cy - 4, 13, 2, C_BLACK);
        gfx_fill(cx - 5, cy - 2, 4, 2, C_BLACK);
        gfx_fill(cx + 2, cy - 2, 4, 2, C_BLACK);
    } else {
        gfx_fill(cx - 4, cy - 3, 2, 2, C_BLACK);
        gfx_fill(cx + 3, cy - 3, 2, 2, C_BLACK);
    }
    gfx_fill(cx - 4, cy + 3, 1, 1, C_BLACK);
    gfx_fill(cx + 4, cy + 3, 1, 1, C_BLACK);
    gfx_fill(cx - 3, cy + 4, 7, 1, C_BLACK);
}

/* a dark box with three red digits, for the mine count and the timer. */
static void draw_counter(int x, int y, int value)
{
    if (value < 0)
        value = 0;
    if (value > 999)
        value = 999;
    char s[4];
    ksnprintf(s, sizeof(s), "%03d", value);
    gfx_fill(x, y, 40, 22, 0x200808);
    gfx_bevel(x, y, 40, 22, false);
    ui_text_bold(x + (40 - ui_text_width(s, true)) / 2, y + 3, s, 0xFF3020);
}

static void draw(struct window *win, int cx, int cy, int cw, int ch)
{
    (void)win, (void)ch;
    if (!cols)
        new_game(0);

    for (int i = 0; i < 2; i++)
        ui_button(cx + levels[i].button_x, cy + LEVEL_Y, levels[i].button_w, BUTTON_H, levels[i].label, i == level);

    int bx = cx + grid_x(), bw = cols * CELL;
    gfx_bevel(bx - 2, cy + BAR_Y, bw + 4, BAR_H, false);
    draw_counter(bx + 2, cy + BAR_Y + 4, mine_count - flags_placed);
    draw_face(cx + (cw - FACE) / 2, cy + BAR_Y + 2);
    draw_counter(bx + bw - 42, cy + BAR_Y + 4, elapsed());

    gfx_bevel(bx - 2, cy + GRID_Y - 2, bw + 4, rows * CELL + 4, false);
    for (int i = 0; i < cols * rows; i++)
        draw_cell(i, bx + i % cols * CELL, cy + GRID_Y + i / cols * CELL);
}

/* ---- input ---- */

static int cell_at(int x, int y)
{
    int gx = x - grid_x(), gy = y - GRID_Y;
    if (gx < 0 || gy < 0 || gx >= cols * CELL || gy >= rows * CELL)
        return -1;
    return gy / CELL * cols + gx / CELL;
}

/*
 * a click: the level buttons and the face both start a new game, and a click
 * on the board lays the mines (on the first click) and uncovers a square, or
 * uncovers the neighbours if you double clicked a number.
 */
static bool click(struct window *win, int x, int y, bool double_click)
{
    if (y >= LEVEL_Y && y < LEVEL_Y + BUTTON_H) {
        for (int i = 0; i < 2; i++)
            if (x >= levels[i].button_x && x < levels[i].button_x + levels[i].button_w)
                new_game(i);
        return true;
    }

    int fx = (win->cw - FACE) / 2;
    if (x >= fx && x < fx + FACE && y >= BAR_Y + 2 && y < BAR_Y + 2 + FACE) {
        new_game(level);
        return true;
    }

    int i = cell_at(x, y);
    if (i < 0 || state != PLAYING)
        return false;
    if (!mines_laid)
        lay_mines(i);
    if (open[i] && double_click)
        chord(i);
    else
        uncover(i);
    return true;
}

static bool right_click(struct window *win, int x, int y)
{
    (void)win;
    int i = cell_at(x, y);
    if (i < 0 || state != PLAYING || open[i])
        return false;
    flag[i] = !flag[i];
    flags_placed += flag[i] ? 1 : -1;
    return true;
}
