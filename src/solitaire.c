#include <apps.h>
#include <cpu.h>
#include <gfx.h>
#include <heap.h>
#include <image.h>
#include <string.h>

/*
 * solitaire: klondike, drawn with the cards from the user's solitaire design.
 * seven columns (the tableau) are dealt out with the top card of each one
 * face up, and the rest of the cards go in the stock, which deals one or
 * three at a time onto the waste. you build the columns downwards in
 * alternating colours (only a king can go in an empty column), and the four
 * foundations upwards from ace to king, one suit each. fill all four
 * foundations and you've won.
 *
 * you drag cards around with the mouse. double clicking a card sends it up to
 * its foundation, and a right click sends up everything that can go. once
 * every card is face up, the rest of the game plays itself. scoring works
 * like windows solitaire: points for moving cards up and turning them over, a
 * penalty for going through the deck again, and a time bonus if you win
 * quickly.
 *
 * the card pictures come in two sheets (see tools/make-solitaire-cards.sh),
 * which get cut up into separate cards in memory the first time the game
 * opens.
 */

#define CARD_W     71
#define CARD_H     99
#define MENU_H     20
#define STATUS_H   20
#define TOP_PAD    10
#define ROW_GAP    12
#define WASTE_FAN  14
#define DOWN_GAP   8
#define UP_GAP     18
#define MIN_DOWN   3
#define MIN_UP     9

#define FACE_UP    0x80
#define CARD(c)    ((c) & 0x3F)
#define SUIT(c)    (CARD(c) / 13)       /* clubs, diamonds, hearts, spades: the rows of the sheet */
#define RANK(c)    (CARD(c) % 13)       /* 0 is the ace and 12 is the king */
#define RED(c)     (SUIT(c) == 1 || SUIT(c) == 2)

enum { STOCK, WASTE, FOUND, TAB = FOUND + 4, PILES = TAB + 7 };

/*
 * the foundations from left to right, the same as the design: spades, hearts,
 * diamonds, clubs
 */
static const int slot_suit[4] = { 3, 2, 1, 0 };

/* what's on the extras sheet */
enum { BACK_GREEN, BACK_BLUE, SLOT_EMPTY, SLOT_STOCK, SLOT_CLUBS };

struct pile {
    int n;
    uint8_t c[52];
};

struct game {
    struct pile p[PILES];
    int score;
    int fan;            /* how many waste cards are fanned out (from the last deal) */
    int passes;         /* how many times we've been through the stock */
    bool started;       /* the clock starts on the first move */
};

static struct game g;
#define UNDO_MAX 64
static struct game undo_stack[UNDO_MAX];
static int undo_count;

static bool ready, won, autofinish;
static int draw_count = 1;
static int back = BACK_GREEN;
static uint32_t start_tick, end_tick;
static int penalized_tens;      /* how many 10 second time penalties we've taken so far */

static struct image *faces[52], *extras[8];
static bool images_tried;

/* dragging */
static int drag_pile = -1, drag_index, drag_dx, drag_dy, drag_x, drag_y, press_x, press_y;
static bool dragging;

/* the menus */
static int open_menu = -1, hover_item = -1;

/* the winning cascade */
static uint32_t *trail;
static int trail_w, trail_h;
static int cascade_slot, cascade_card = -1, cascade_left[4];   /* the game itself is left alone while this plays */
static int bounce_x, bounce_y, bounce_vx, bounce_vy;
static bool cascading, cascade_done;

static void draw(struct window *win, int cx, int cy, int cw, int ch);
static bool key(struct window *win, char c);
static bool click(struct window *win, int x, int y, bool double_click);
static bool drag(struct window *win, int x, int y);
static bool release(struct window *win);
static bool hover(struct window *win, int x, int y);
static bool right_click(struct window *win, int x, int y);
static bool tick(struct window *win);

struct window solitaire_window = {
    .title = "Solitaire", .proc = "solitaire", .icon = "solitaire", .x = 16, .y = 4, .w = 608, .h = 434,
    .min_w = 568, .min_h = 380, .resizable = true,
    .draw = draw, .key = key, .click = click, .drag = drag, .release = release, .hover = hover,
    .right_click = right_click, .tick = tick,
};

/* ---- the card pictures ---- */

/* copies a card sized piece out of a sheet into a picture of its own */
static struct image *cut(const struct image *sheet, int col, int row)
{
    struct image *img = kmalloc(sizeof(*img));
    uint32_t *px = kmalloc(CARD_W * CARD_H * 4);
    if (!img || !px || (col + 1) * CARD_W > sheet->width || (row + 1) * CARD_H > sheet->height) {
        kfree(img);
        kfree(px);
        return 0;
    }
    for (int y = 0; y < CARD_H; y++)
        memcpy(px + y * CARD_W, sheet->pixels + (row * CARD_H + y) * sheet->width + col * CARD_W, CARD_W * 4);
    img->width = CARD_W;
    img->height = CARD_H;
    img->pixels = px;
    return img;
}

static void load_cards(void)
{
    if (images_tried)
        return;
    images_tried = true;
    struct image *sheet = image_load("/usr/share/solitaire/cards.cimg");
    if (sheet) {
        for (int i = 0; i < 52; i++)
            faces[i] = cut(sheet, i % 13, i / 13);
        image_free(sheet);
    }
    sheet = image_load("/usr/share/solitaire/extras.cimg");
    if (sheet) {
        for (int i = 0; i < 8; i++)
            extras[i] = cut(sheet, i, 0);
        image_free(sheet);
    }
}

/* ---- dealing ---- */

static uint32_t rng;

static uint32_t next_random(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void stop_cascade(void)
{
    cascading = false;
    cascade_done = false;
    cascade_card = -1;
    kfree(trail);
    trail = 0;
}

/*
 * deals a new game. the deck gets shuffled (fisher-yates, seeded from the
 * clock), then dealt into the seven columns, one card in
 * the first up to seven in the last with only the top one face up, and the
 * rest go in the stock.
 */
static void new_game(void)
{
    stop_cascade();
    memset(&g, 0, sizeof(g));
    int hour, minute, second;
    rtc_time(&hour, &minute, &second);
    rng = (timer_ticks() * 2654435761u ^ (uint32_t)(hour * 3600 + minute * 60 + second)) | 1;
    uint8_t deck[52];
    for (int i = 0; i < 52; i++)
        deck[i] = i;
    for (int i = 51; i > 0; i--) {
        int j = next_random() % (i + 1);
        uint8_t t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    int k = 0;
    for (int col = 0; col < 7; col++) {
        struct pile *p = &g.p[TAB + col];
        for (int i = 0; i <= col; i++)
            p->c[p->n++] = deck[k++] | (i == col ? FACE_UP : 0);
    }
    while (k < 52)
        g.p[STOCK].c[g.p[STOCK].n++] = deck[k++];
    undo_count = 0;
    won = autofinish = cascade_done = false;
    penalized_tens = 0;
    drag_pile = -1;
    dragging = false;
    ready = true;
}

static uint32_t elapsed(void)
{
    if (!g.started)
        return 0;
    return ((won ? end_tick : timer_ticks()) - start_tick) / timer_hz();
}

static void add_score(int points)
{
    g.score += points;
    if (g.score < 0)
        g.score = 0;
}

static void save_undo(void)
{
    if (undo_count == UNDO_MAX) {
        memmove(undo_stack, undo_stack + 1, sizeof(struct game) * (UNDO_MAX - 1));
        undo_count--;
    }
    undo_stack[undo_count++] = g;
    if (!g.started) {
        g.started = true;
        start_tick = timer_ticks();
    }
}

static void undo(void)
{
    if (!undo_count || won)
        return;
    bool started = g.started;
    g = undo_stack[--undo_count];
    g.started = started;    /* the clock keeps going */
    autofinish = false;
    /*
     * AI Contribution (Bug Fix): the board just changed under any cards being
     * held, so let go of them. otherwise letting go of the mouse later used a
     * pile and position from before the undo.
     */
    dragging = false;
    drag_pile = -1;
}

/* ---- the rules ---- */

static struct pile *pile(int i)
{
    return &g.p[i];
}

static int top(int i)
{
    struct pile *p = pile(i);
    return p->n ? p->c[p->n - 1] : -1;
}

/* whether the cards from index onwards in pile src can be picked up together */
static bool can_lift(int src, int index)
{
    struct pile *p = pile(src);
    if (index < 0 || index >= p->n || !(p->c[index] & FACE_UP))
        return false;
    if (src == STOCK)
        return false;
    if (src < TAB)
        return index == p->n - 1;
    for (int i = index; i + 1 < p->n; i++) {
        int a = p->c[i], b = p->c[i + 1];
        if (!(b & FACE_UP) || RED(a) == RED(b) || RANK(a) != RANK(b) + 1)
            return false;
    }
    return true;
}

/* whether card (the first of count cards) can go on pile dst */
static bool can_place(int card, int count, int dst)
{
    if (dst >= FOUND && dst < TAB) {
        int suit = slot_suit[dst - FOUND];
        return count == 1 && SUIT(card) == suit && RANK(card) == pile(dst)->n;
    }
    if (dst >= TAB) {
        int t = top(dst);
        if (t < 0)
            return RANK(card) == 12;
        return (t & FACE_UP) && RED(t) != RED(card) && RANK(t) == RANK(card) + 1;
    }
    return false;
}

static void check_won(void);

/* turns the top card of a column over, if it's face down */
static void flip_top(int col)
{
    struct pile *p = pile(col);
    if (col >= TAB && p->n && !(p->c[p->n - 1] & FACE_UP)) {
        p->c[p->n - 1] |= FACE_UP;
        add_score(5);
    }
}

static void move_cards(int src, int index, int dst)
{
    struct pile *s = pile(src), *d = pile(dst);
    int count = s->n - index;
    for (int i = 0; i < count; i++)
        d->c[d->n++] = s->c[index + i];
    s->n = index;

    if (dst >= FOUND && dst < TAB && (src == WASTE || src >= TAB))
        add_score(10);
    else if (src == WASTE && dst >= TAB)
        add_score(5);
    else if (src >= FOUND && src < TAB && dst >= TAB)
        add_score(-15);
    if (src == WASTE && g.fan > 1)
        g.fan--;
    flip_top(src);
    check_won();
}

/* the foundation a card can go to right now, or -1 */
static int foundation_for(int card)
{
    for (int f = FOUND; f < TAB; f++)
        if (can_place(card, 1, f))
            return f;
    return -1;
}

static bool send_up(int src)
{
    int t = top(src);
    if (t < 0 || !(t & FACE_UP))
        return false;
    int f = foundation_for(t);
    if (f < 0 || src == f)
        return false;
    save_undo();
    move_cards(src, pile(src)->n - 1, f);
    return true;
}

/* deals from the stock, or turns the waste back over when the stock runs out */
static void deal(void)
{
    struct pile *s = pile(STOCK), *w = pile(WASTE);
    if (!s->n && !w->n)
        return;
    save_undo();
    if (!s->n) {
        while (w->n)
            s->c[s->n++] = CARD(w->c[--w->n]);
        g.fan = 0;
        g.passes++;
        add_score(draw_count == 1 ? -100 : -20);
        return;
    }
    int k = s->n < draw_count ? s->n : draw_count;
    for (int i = 0; i < k; i++)
        w->c[w->n++] = s->c[--s->n] | FACE_UP;
    g.fan = k;
}

static bool all_face_up(void)
{
    if (pile(STOCK)->n || pile(WASTE)->n)
        return false;
    for (int col = TAB; col < PILES; col++)
        for (int i = 0; i < pile(col)->n; i++)
            if (!(pile(col)->c[i] & FACE_UP))
                return false;
    return true;
}

static void start_cascade(void);

static void check_won(void)
{
    for (int f = FOUND; f < TAB; f++)
        if (pile(f)->n != 13)
            return;
    won = true;
    autofinish = false;
    end_tick = timer_ticks();
    uint32_t secs = elapsed();
    if (secs >= 30)
        add_score(700000 / secs);
    start_cascade();
}

/* ---- layout ---- */

static int gap_x(int cw)
{
    int gap = (cw - 7 * CARD_W) / 8;
    return gap < 4 ? 4 : gap;
}

static int col_x(int cw, int col)
{
    int gap = gap_x(cw);
    return gap + col * (CARD_W + gap);
}

static int top_y(void)
{
    return MENU_H + TOP_PAD;
}

static int tab_y(void)
{
    return top_y() + CARD_H + ROW_GAP;
}

/*
 * where a pile sits: the stock and waste on the left, the foundations in
 * columns 3 to 6, then the tableau underneath
 */
static void pile_origin(int cw, int i, int *x, int *y)
{
    if (i == STOCK) {
        *x = col_x(cw, 0);
        *y = top_y();
    } else if (i == WASTE) {
        *x = col_x(cw, 1);
        *y = top_y();
    } else if (i < TAB) {
        *x = col_x(cw, 3 + i - FOUND);
        *y = top_y();
    } else {
        *x = col_x(cw, i - TAB);
        *y = tab_y();
    }
}

/*
 * the spacing between cards in a column, squeezed up if needed so the column
 * fits above the status bar
 */
static void column_gaps(int ch, int col, int *down, int *up)
{
    struct pile *p = pile(col);
    int ndown = 0, nup = 0;
    for (int i = 0; i + 1 < p->n; i++) {
        if (!(p->c[i] & FACE_UP))
            ndown++;
        else
            nup++;
    }
    int room = ch - STATUS_H - 6 - tab_y() - CARD_H;
    *down = DOWN_GAP;
    *up = UP_GAP;
    if (ndown * *down + nup * *up <= room)
        return;
    if (nup) {
        *up = (room - ndown * *down) / nup;
        if (*up >= MIN_UP)
            return;
        *up = MIN_UP;
    }
    if (ndown) {
        *down = (room - nup * *up) / ndown;
        if (*down < MIN_DOWN)
            *down = MIN_DOWN;
    }
}

/* the top left corner of card number index in a pile */
static void card_pos(int cw, int ch, int i, int index, int *x, int *y)
{
    pile_origin(cw, i, x, y);
    struct pile *p = pile(i);
    if (i == WASTE) {
        int shown = g.fan < 1 ? 1 : g.fan;
        if (shown > p->n)
            shown = p->n;
        int first = p->n - shown;
        if (index > first)
            *x += (index - first) * WASTE_FAN;
    } else if (i >= TAB) {
        int down, up;
        column_gaps(ch, i, &down, &up);
        for (int k = 0; k < index; k++)
            *y += (p->c[k] & FACE_UP) ? up : down;
    }
}

/*
 * the card under a point: which pile and which card (-1 for an empty pile's
 * slot). false if there's nothing there.
 */
static bool hit(int cw, int ch, int x, int y, int *pi, int *index)
{
    for (int i = 0; i < PILES; i++) {
        struct pile *p = pile(i);
        int first = 0;
        if (i == STOCK || (i >= FOUND && i < TAB))
            first = p->n ? p->n - 1 : 0;
        if (i == WASTE)
            first = p->n ? p->n - 1 : 0;
        /* the top card is drawn last, so it gets checked first */
        for (int k = p->n - 1; k >= first; k--) {
            int px, py;
            card_pos(cw, ch, i, k, &px, &py);
            if (x >= px && x < px + CARD_W && y >= py && y < py + CARD_H) {
                *pi = i;
                *index = k;
                return true;
            }
        }
        if (!p->n) {
            int px, py;
            pile_origin(cw, i, &px, &py);
            if (x >= px && x < px + CARD_W && y >= py && y < py + CARD_H) {
                *pi = i;
                *index = -1;
                return true;
            }
        }
    }
    return false;
}

/* ---- the menus ---- */

enum { M_NEW, M_UNDO, M_DRAW1, M_DRAW3, M_GREEN, M_BLUE, M_EXIT, M_HOWTO, M_ABOUT, M_SEP };

struct menu_item {
    const char *label, *shortcut;
    int action;
};

static const struct menu_item game_items[] = {
    { "New Game", "N", M_NEW }, { 0, 0, M_SEP }, { "Undo", "U", M_UNDO }, { 0, 0, M_SEP },
    { "Draw One", 0, M_DRAW1 }, { "Draw Three", 0, M_DRAW3 }, { 0, 0, M_SEP },
    { "Green Card Back", 0, M_GREEN }, { "Blue Card Back", 0, M_BLUE }, { 0, 0, M_SEP },
    { "Exit", 0, M_EXIT },
};
static const struct menu_item help_items[] = {
    { "How to Play", 0, M_HOWTO }, { "About Solitaire", 0, M_ABOUT },
};

static const struct {
    const char *title;
    int x, w;
    const struct menu_item *items;
    int count;
} menus[2] = {
    { "Game", 4, 46, game_items, sizeof(game_items) / sizeof(game_items[0]) },
    { "Help", 50, 40, help_items, sizeof(help_items) / sizeof(help_items[0]) },
};

#define DROP_W   172
#define ITEM_H   22
#define SEP_H    7

static int item_y(int m, int index)
{
    int y = MENU_H + 3;
    for (int i = 0; i < index; i++)
        y += menus[m].items[i].action == M_SEP ? SEP_H : ITEM_H;
    return y;
}

static int menu_height(int m)
{
    return item_y(m, menus[m].count) - MENU_H + 3;
}

static int menu_at(int x, int y)
{
    if (y < 0 || y >= MENU_H)
        return -1;
    for (int m = 0; m < 2; m++)
        if (x >= menus[m].x && x < menus[m].x + menus[m].w)
            return m;
    return -1;
}

static int item_at(int x, int y)
{
    if (open_menu < 0)
        return -1;
    int mx = menus[open_menu].x;
    if (x < mx || x >= mx + DROP_W)
        return -1;
    for (int i = 0; i < menus[open_menu].count; i++) {
        int iy = item_y(open_menu, i), h = menus[open_menu].items[i].action == M_SEP ? SEP_H : ITEM_H;
        if (y >= iy && y < iy + h)
            return menus[open_menu].items[i].action == M_SEP ? -1 : i;
    }
    return -1;
}

static bool item_checked(int action)
{
    return (action == M_DRAW1 && draw_count == 1) || (action == M_DRAW3 && draw_count == 3) ||
           (action == M_GREEN && back == BACK_GREEN) || (action == M_BLUE && back == BACK_BLUE);
}

static bool item_enabled(int action)
{
    return action != M_UNDO || (undo_count && !won);
}

static int wanted_draw;

static void switch_draw(void *ctx)
{
    (void)ctx;
    draw_count = wanted_draw;
    new_game();
    gui_poke();
}

/*
 * carries out a menu item: new game, undo, switching between drawing one and
 * three cards, picking the colour of the card backs, closing the game, or
 * showing the how to play and about boxes.
 */
static void do_action(int action)
{
    switch (action) {
    case M_NEW:
        new_game();
        break;
    case M_UNDO:
        undo();
        break;
    case M_DRAW1:
    case M_DRAW3:
        wanted_draw = action == M_DRAW1 ? 1 : 3;
        if (wanted_draw == draw_count)
            break;
        /*
         * changing the draw changes the whole game, so it means starting over
         */
        if (!g.started || won) {
            switch_draw(0);
        } else {
            gui_confirm(ICON_QUESTION, "Solitaire",
                        wanted_draw == 3 ? "Start a new game, drawing three cards at a time?"
                                         : "Start a new game, drawing one card at a time?",
                        switch_draw, 0);
        }
        break;
    case M_GREEN:
        back = BACK_GREEN;
        break;
    case M_BLUE:
        back = BACK_BLUE;
        break;
    case M_EXIT:
        gui_close(&solitaire_window);
        break;
    case M_HOWTO:
        gui_message(ICON_INFO, "How to Play",
                    "Put every card on the four piles at the top right, ace up to king in each suit. "
                    "Build the seven columns down in alternating colors; only a king goes in an empty one. "
                    "Click the deck to deal. Drag cards to move them, double-click to send one up, "
                    "and right-click to send up everything that can go.");
        break;
    case M_ABOUT:
        gui_message(ICON_INFO, "About Solitaire",
                    "Solitaire for CraftOS, with cards from the CraftOS Solitaire design. "
                    "Scoring: 10 for each card to the top, 5 for a card from the deck to a column, "
                    "5 for turning one over, and a bonus for winning quickly.");
        break;
    }
}

/* ---- the winning cascade ---- */

/* draws a card into the trail picture, blending in its rounded corners */
static void trail_blit(const struct image *img, int x, int y)
{
    if (!trail || !img)
        return;
    for (int py = 0; py < img->height; py++) {
        int ty = y + py;
        if (ty < 0 || ty >= trail_h)
            continue;
        for (int px = 0; px < img->width; px++) {
            int tx = x + px;
            if (tx < 0 || tx >= trail_w)
                continue;
            uint32_t s = img->pixels[py * img->width + px], a = s >> 24;
            if (!a)
                continue;
            uint32_t *d = &trail[ty * trail_w + tx];
            if (a == 255) {
                *d = s;
                continue;
            }
            uint32_t out = 0xFF000000u;
            for (int sh = 0; sh < 24; sh += 8) {
                uint32_t sc = (s >> sh) & 0xFF, dc = (*d >> sh) & 0xFF;
                out |= ((sc * a + dc * (255 - a)) / 255) << sh;
            }
            *d = out;
        }
    }
}

static uint32_t felt_color(int y, int ch)
{
    return ui_mix(0x2C9A55, 0x0F5A2C, y, ch);
}

/*
 * the winning show: the cards leave the foundations one at a time, kings
 * first, and bounce off across the table, each one leaving a trail of copies
 * of itself behind.
 */
static void start_cascade(void)
{
    int cw = solitaire_window.cw, ch = solitaire_window.ch;
    trail_w = cw;
    trail_h = ch - MENU_H - STATUS_H;
    if (trail_w <= 0 || trail_h <= 0)
        return;
    trail = kmalloc((size_t)trail_w * trail_h * 4);
    if (!trail)
        return;
    for (int y = 0; y < trail_h; y++)
        for (int x = 0; x < trail_w; x++)
            trail[y * trail_w + x] = 0xFF000000u | felt_color(y + MENU_H, ch);
    /* the table as it is now: the empty stock and the full foundations */
    int x, y;
    pile_origin(cw, STOCK, &x, &y);
    trail_blit(extras[SLOT_STOCK], x, y - MENU_H);
    for (int f = FOUND; f < TAB; f++) {
        pile_origin(cw, f, &x, &y);
        trail_blit(faces[CARD(top(f))], x, y - MENU_H);
    }
    for (int col = TAB; col < PILES; col++) {
        pile_origin(cw, col, &x, &y);
        trail_blit(extras[SLOT_EMPTY], x, y - MENU_H);
    }
    cascading = true;
    cascade_slot = 0;
    cascade_card = -1;
    for (int i = 0; i < 4; i++)
        cascade_left[i] = 13;
}

/*
 * sends the next card off on its bounce. it goes round the foundations from
 * left to right taking the top card off each, gives it a random speed and
 * direction, and stops the show once every foundation is empty.
 */
static void launch_next(void)
{
    /*
     * the next card: go round the foundations from left to right, taking the
     * top card of each
     */
    int tries = 0;
    while (tries < 4 && !cascade_left[cascade_slot]) {
        cascade_slot = (cascade_slot + 1) % 4;
        tries++;
    }
    if (!cascade_left[cascade_slot]) {
        cascading = false;
        cascade_done = true;
        return;
    }
    int x, y;
    pile_origin(trail_w, FOUND + cascade_slot, &x, &y);
    cascade_card = CARD(pile(FOUND + cascade_slot)->c[--cascade_left[cascade_slot]]);
    /* the foundation shows the card underneath from now on */
    if (cascade_left[cascade_slot])
        trail_blit(faces[CARD(pile(FOUND + cascade_slot)->c[cascade_left[cascade_slot] - 1])], x, y - MENU_H);
    else
        trail_blit(extras[SLOT_CLUBS + slot_suit[cascade_slot]], x, y - MENU_H);
    bounce_x = x;
    bounce_y = y - MENU_H;
    int speed = 3 + next_random() % 6;
    bounce_vx = (next_random() & 1) ? speed : -speed;
    bounce_vy = -(int)(next_random() % 9);
    cascade_slot = (cascade_slot + 1) % 4;
}

static void cascade_step(void)
{
    if (cascade_card < 0)
        launch_next();
    if (cascade_card < 0)
        return;
    bounce_x += bounce_vx;
    bounce_vy += 1;
    bounce_y += bounce_vy;
    if (bounce_y + CARD_H > trail_h) {
        bounce_y = trail_h - CARD_H;
        bounce_vy = -bounce_vy * 3 / 4;
    }
    trail_blit(faces[cascade_card], bounce_x, bounce_y);
    if (bounce_x + CARD_W < 0 || bounce_x > trail_w)
        cascade_card = -1;
}

static void show_win(void)
{
    char msg[128];
    uint32_t secs = elapsed();
    ksnprintf(msg, sizeof(msg), "You won! Score: %d, in %u:%02u. Choose New Game from the Game menu to play again.",
              g.score, secs / 60, secs % 60);
    gui_message(ICON_INFO, "Solitaire", msg);
}

/* ---- drawing ---- */

static void draw_card(int x, int y, int card)
{
    const struct image *img = (card & FACE_UP) ? faces[CARD(card)] : extras[back];
    if (img) {
        gfx_blit(x, y, img);
        return;
    }
    /* no pictures: just a plain card with its rank and suit on it */
    static const char ranks[] = "A23456789TJQK", suits[] = "CDHS";
    gfx_fill(x, y, CARD_W, CARD_H, (card & FACE_UP) ? C_WHITE : 0x2A6EB0);
    gfx_rect(x, y, CARD_W, CARD_H, C_INK);
    if (card & FACE_UP) {
        char label[3] = { ranks[RANK(card)], suits[SUIT(card)], 0 };
        ui_text_bold(x + 4, y + 2, label, RED(card) ? 0xC8201C : C_BLACK);
    }
}

static void draw_slot(int x, int y, int which)
{
    if (extras[which])
        gfx_blit(x, y, extras[which]);
    else
        gfx_rect(x, y, CARD_W, CARD_H, 0x6FB487);
}

/*
 * draws the menu bar along the top, and whichever menu is open, with a shadow
 * under it. items that can't be used right now (like undo with nothing to
 * undo) are greyed out.
 */
static void draw_menus(int cx, int cy, int cw)
{
    gfx_fill(cx, cy, cw, MENU_H, C_FACE);
    gfx_fill(cx, cy + MENU_H - 1, cw, 1, C_SHADOW);
    for (int m = 0; m < 2; m++) {
        if (m == open_menu)
            gfx_fill(cx + menus[m].x, cy + 1, menus[m].w, MENU_H - 2, C_SELECT);
        ui_text(cx + menus[m].x + 8, cy + 2, menus[m].title, m == open_menu ? C_WHITE : C_INK);
    }
    if (open_menu < 0)
        return;
    int x = cx + menus[open_menu].x, y = cy + MENU_H, h = menu_height(open_menu);
    gfx_fill_alpha(x + 3, y + 3, DROP_W, h, 0x50000000);
    gfx_fill(x, y, DROP_W, h, C_LIGHT);
    gfx_rect(x, y, DROP_W, h, C_SHADOW);
    for (int i = 0; i < menus[open_menu].count; i++) {
        const struct menu_item *it = &menus[open_menu].items[i];
        int iy = cy + item_y(open_menu, i);
        if (it->action == M_SEP) {
            gfx_fill(x + 6, iy + 3, DROP_W - 12, 1, C_SHADOW);
            continue;
        }
        bool enabled = item_enabled(it->action), lit = i == hover_item && enabled;
        if (lit)
            gfx_fill(x + 2, iy, DROP_W - 4, ITEM_H, C_SELECT);
        uint32_t ink = !enabled ? C_SHADOW : lit ? C_WHITE : C_INK;
        if (item_checked(it->action))
            gfx_fill(x + 10, iy + 9, 5, 5, ink);
        ui_text(x + 22, iy + 3, it->label, ink);
        if (it->shortcut)
            ui_text(x + DROP_W - 12 - ui_text_width(it->shortcut, false), iy + 3, it->shortcut, lit ? C_WHITE : C_SHADOW);
    }
}

/*
 * draws the whole game: the felt (or the cascade's trail picture, during the
 * winning show), every pile, any cards being dragged on top of everything,
 * the menu bar, and the status bar with the score and the time.
 */
static void draw(struct window *win, int cx, int cy, int cw, int ch)
{
    (void)win;
    load_cards();
    if (!ready)
        new_game();

    /* the felt, a little lighter at the top */
    int felt_h = ch - MENU_H - STATUS_H;
    if (trail && (trail_w != cw || trail_h != felt_h))
        stop_cascade();     /* the window changed size, so go back to the plain table */
    if (trail) {
        struct image view = { trail_w, trail_h, trail };
        gfx_blit(cx, cy + MENU_H, &view);
    }
    if (!trail) {
        for (int y = 0; y < felt_h; y++)
            gfx_fill(cx, cy + MENU_H + y, cw, 1, felt_color(MENU_H + y, ch));

        for (int i = 0; i < PILES; i++) {
            struct pile *p = pile(i);
            int x, y;
            pile_origin(cw, i, &x, &y);
            if (i == STOCK) {
                if (p->n)
                    draw_card(cx + x, cy + y, p->c[p->n - 1] & ~FACE_UP);
                else
                    draw_slot(cx + x, cy + y, SLOT_STOCK);
                continue;
            }
            if (i >= FOUND && i < TAB) {
                int shown = p->n;
                if (drag_pile == i && dragging)
                    shown--;
                if (shown > 0)
                    draw_card(cx + x, cy + y, p->c[shown - 1]);
                else
                    draw_slot(cx + x, cy + y, SLOT_CLUBS + slot_suit[i - FOUND]);
                continue;
            }
            if (i >= TAB && !p->n) {
                draw_slot(cx + x, cy + y, SLOT_EMPTY);
                continue;
            }
            int first = 0;
            if (i == WASTE) {
                int fan = g.fan < 1 ? 1 : g.fan;
                first = p->n > fan ? p->n - fan : 0;
                /*
                 * the card underneath the fan, so the pile still looks like a
                 * pile
                 */
                if (first > 0)
                    draw_card(cx + x, cy + y, p->c[first - 1]);
            }
            int last = (dragging && drag_pile == i) ? drag_index : p->n;
            for (int k = first; k < last; k++) {
                int px, py;
                card_pos(cw, ch, i, k, &px, &py);
                draw_card(cx + px, cy + py, p->c[k]);
            }
        }

        /* the cards being dragged, on top of everything */
        if (dragging && drag_pile >= 0) {
            struct pile *p = pile(drag_pile);
            for (int k = drag_index; k < p->n; k++)
                draw_card(cx + drag_x - drag_dx, cy + drag_y - drag_dy + (k - drag_index) * UP_GAP, p->c[k]);
        }
    }

    /* the status bar */
    int sy = cy + ch - STATUS_H;
    gfx_fill(cx, sy, cw, STATUS_H, C_FACE);
    gfx_fill(cx, sy, cw, 1, C_SHADOW);
    char score[32], time[32];
    uint32_t secs = elapsed();
    ksnprintf(score, sizeof(score), "Score: %d", g.score);
    ksnprintf(time, sizeof(time), "Time: %u:%02u", secs / 60, secs % 60);
    int tw = ui_text_width(time, false);
    ui_text(cx + cw - 10 - tw, sy + 2, time, C_INK);
    ui_text(cx + cw - 30 - tw - ui_text_width(score, false), sy + 2, score, C_INK);
    ui_text(cx + 8, sy + 2, won ? "You won!" : draw_count == 1 ? "Draw one" : "Draw three", C_SHADOW);

    draw_menus(cx, cy, cw);
}

/* ---- input ---- */

static bool menu_click(int x, int y)
{
    int m = menu_at(x, y);
    if (m >= 0) {
        open_menu = open_menu == m ? -1 : m;
        hover_item = -1;
        return true;
    }
    if (open_menu < 0)
        return false;
    int i = item_at(x, y);
    int action = i >= 0 ? menus[open_menu].items[i].action : -1;
    open_menu = -1;
    hover_item = -1;
    if (action >= 0 && item_enabled(action))
        do_action(action);
    return true;
}

/*
 * a click. the menus come first, and during the winning show a click just
 * skips to the end. otherwise it deals from the stock, sends a double clicked
 * card up to its foundation, turns over a face down card, or picks up cards
 * ready to drag.
 */
static bool click(struct window *win, int x, int y, bool double_click)
{
    if (menu_click(x, y))
        return true;
    if (cascading || (won && cascade_done)) {
        /* a click ends the show */
        if (cascading) {
            cascading = false;
            cascade_done = true;
            show_win();
        }
        return true;
    }
    if (won)
        return false;
    int pi, index;
    if (!hit(win->cw, win->ch, x, y, &pi, &index))
        return false;
    if (pi == STOCK) {
        deal();
        return true;
    }
    if (double_click && index >= 0 && index == pile(pi)->n - 1 && pi != STOCK) {
        dragging = false;
        drag_pile = -1;
        return send_up(pi);
    }
    if (index >= 0 && pi >= TAB && !(pile(pi)->c[index] & FACE_UP) && index == pile(pi)->n - 1) {
        save_undo();
        flip_top(pi);
        return true;
    }
    if (index >= 0 && can_lift(pi, index)) {
        int px, py;
        card_pos(win->cw, win->ch, pi, index, &px, &py);
        drag_pile = pi;
        drag_index = index;
        drag_dx = x - px;
        drag_dy = y - py;
        drag_x = press_x = x;
        drag_y = press_y = y;
        dragging = false;
    }
    return false;
}

static bool drag(struct window *win, int x, int y)
{
    (void)win;
    if (drag_pile < 0)
        return false;
    drag_x = x;
    drag_y = y;
    int dx = x - press_x, dy = y - press_y;
    if (!dragging && dx * dx + dy * dy > 9)
        dragging = true;
    return dragging;
}

/* how much two cards overlap, for working out where a dropped card goes */
static int overlap(int ax, int ay, int bx, int by)
{
    int w = CARD_W - (ax > bx ? ax - bx : bx - ax), h = CARD_H - (ay > by ? ay - by : by - ay);
    return w > 0 && h > 0 ? w * h : 0;
}

/*
 * letting go of the mouse. if cards were being dragged, they go to whichever
 * pile they cover the most that will take them, as one step you can undo.
 * if nothing will take them, they just go back where they came from.
 */
static bool release(struct window *win)
{
    if (drag_pile < 0)
        return false;
    bool was_dragging = dragging;
    int src = drag_pile, index = drag_index;
    dragging = false;
    drag_pile = -1;
    if (!was_dragging)
        return true;
    /*
     * the pile the dropped card covers the most, out of the ones that will
     * take it
     */
    int x = drag_x - drag_dx, y = drag_y - drag_dy;
    /*
     * AI Contribution (Bug Fix): if the pile changed while the cards were in
     * the air, the index can point past its end, and reading the card there
     * (and moving that many) runs off the pile
     */
    if (index < 0 || index >= pile(src)->n)
        return true;
    int card = pile(src)->c[index], count = pile(src)->n - index;
    int best = -1, best_area = 0;
    for (int i = FOUND; i < PILES; i++) {
        if (i == src || !can_place(card, count, i))
            continue;
        int px, py;
        int n = pile(i)->n;
        if (n && i >= TAB)
            card_pos(win->cw, win->ch, i, n - 1, &px, &py);
        else
            pile_origin(win->cw, i, &px, &py);
        int area = overlap(x, y, px, py);
        if (area > best_area) {
            best_area = area;
            best = i;
        }
    }
    if (best >= 0) {
        save_undo();
        move_cards(src, index, best);
    }
    return true;
}

static bool hover(struct window *win, int x, int y)
{
    (void)win;
    if (open_menu < 0)
        return false;
    int m = menu_at(x, y);
    if (m >= 0 && m != open_menu) {
        open_menu = m;
        hover_item = -1;
        return true;
    }
    int i = item_at(x, y);
    if (i != hover_item) {
        hover_item = i;
        return true;
    }
    return false;
}

/* the waste or column pile whose top card can go up right now, or -1 */
static int playable_up(void)
{
    for (int i = WASTE; i < PILES; i++) {
        if (i >= FOUND && i < TAB)
            continue;
        int t = top(i);
        if (t >= 0 && (t & FACE_UP) && foundation_for(t) >= 0)
            return i;
    }
    return -1;
}

/*
 * a right click sends everything that can go up to the foundations, as one
 * step to undo
 */
static bool right_click(struct window *win, int x, int y)
{
    (void)win;
    (void)x;
    (void)y;
    if (won || open_menu >= 0 || cascade_done || playable_up() < 0)
        return false;
    save_undo();
    for (int i; !won && (i = playable_up()) >= 0;)
        move_cards(i, pile(i)->n - 1, foundation_for(top(i)));
    return true;
}

static bool key(struct window *win, char c)
{
    (void)win;
    if (c == 27 && open_menu >= 0) {
        open_menu = -1;
        return true;
    }
    if (c == 'n' || c == 'N') {
        new_game();
        return true;
    }
    if (c == 'u' || c == 'U' || c == '\b') {
        undo();
        return true;
    }
    return false;
}

/*
 * runs on every trip round the desktop's loop: it ticks the clock and the
 * time penalty along, moves the bouncing cards in the winning show, and plays
 * the next card up when the game's finishing itself.
 */
static bool tick(struct window *win)
{
    (void)win;
    if (!ready)
        return false;
    bool redraw = false;

    /* the clock, and 2 points off every 10 seconds */
    static uint32_t last_secs = 0xFFFFFFFF;
    uint32_t secs = elapsed();
    if (secs != last_secs) {
        last_secs = secs;
        redraw = true;
        if (g.started && !won && (int)(secs / 10) > penalized_tens) {
            penalized_tens = secs / 10;
            add_score(-2);
        }
    }

    static uint32_t last_step;
    uint32_t now = timer_ticks();
    if (cascading && now - last_step >= 2) {
        last_step = now;
        for (int i = 0; i < 3 && cascading; i++)
            cascade_step();
        if (!cascading && cascade_done)
            show_win();
        return true;
    }

    /*
     * every card is face up, so play the rest automatically, one every few
     * ticks
     */
    if (!won && !dragging && open_menu < 0 && all_face_up())
        autofinish = true;
    if (autofinish && !won && !dragging && now - last_step >= 6) {    /* AI Contribution (Bug Fix): not mid-drag */
        last_step = now;
        int best = -1, best_rank = 99;
        for (int i = TAB; i < PILES; i++) {
            int t = top(i);
            if (t >= 0 && foundation_for(t) >= 0 && RANK(t) < best_rank) {
                best_rank = RANK(t);
                best = i;
            }
        }
        if (best >= 0) {
            save_undo();
            move_cards(best, pile(best)->n - 1, foundation_for(top(best)));
        } else {
            autofinish = false;
        }
        return true;
    }
    return redraw;
}
