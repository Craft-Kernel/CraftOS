#include <cpu.h>
#include <io.h>
#include <stdbool.h>
#include <task.h>

/*
 * the ps/2 keyboard, on irq 1, with a us layout (scancode set 1).
 *
 * every key sends a "make" code when it goes down, and the same code with the
 * top bit set when it comes back up. we keep track of shift and caps lock
 * ourselves, turn key presses into ordinary characters, and drop them into a
 * small ring buffer for whoever's reading the keyboard.
 *
 * a few keys don't have a character (the arrows, page up and down, f11 and
 * ctrl+alt+del), so they come through as the key_* codes from cpu.h. the
 * arrows and paging keys are "extended" keys: the keyboard sends 0xe0 first
 * to say so, then the code.
 */

#define KBD_DATA 0x60
#define BUF_SIZE 128

static const char keymap[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ',
};

static const char keymap_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ',
};

#define SC_LSHIFT     0x2A
#define SC_RSHIFT     0x36
#define SC_CAPSLOCK   0x3A
#define SC_EXTENDED   0xE0
#define SC_RELEASE    0x80
#define SC_F11        0x57
#define SC_CTRL       0x1D
#define SC_ALT        0x38
#define SC_DELETE     0x53

static bool shift, caps, extended, ctrl, alt;

/*
 * only the interrupt handler moves head, and only the reader moves tail, so
 * the two never step on each other and no locking is needed. if the buffer
 * fills up because nobody's reading, new keys are simply dropped.
 */
static volatile char buffer[BUF_SIZE];
static volatile uint32_t head, tail;

static void push(char c)
{
    uint32_t next = (head + 1) % BUF_SIZE;
    if (next != tail) {
        buffer[head] = c;
        head = next;
    }
}

static char extended_key(uint8_t sc)
{
    switch (sc) {
    case 0x48: return KEY_UP;
    case 0x50: return KEY_DOWN;
    case 0x49: return KEY_PGUP;
    case 0x51: return KEY_PGDN;
    default:   return 0;
    }
}

/*
 * runs every time the keyboard has something to say. it works out which key
 * it was and whether it went up or down, keeps track of the shift, ctrl, alt
 * and caps lock state, and turns presses into characters (or one of the
 * special key codes) for the buffer.
 */
static void keyboard_irq(struct regs *r)
{
    (void)r;
    uint8_t sc = inb(KBD_DATA);

    if (sc == SC_EXTENDED) {
        extended = true;
        return;
    }

    bool released = sc & SC_RELEASE;
    sc &= ~SC_RELEASE;

    /*
     * the right ctrl and alt keys are just extended versions of the left
     * ones, so they count the same.
     */
    if (sc == SC_CTRL) {
        ctrl = !released;
        extended = false;
        return;
    }
    if (sc == SC_ALT) {
        alt = !released;
        extended = false;
        return;
    }
    if (sc == SC_DELETE && ctrl && alt && !released) {
        push(KEY_CTRL_ALT_DEL);
        extended = false;
        return;
    }

    if (extended) {
        extended = false;
        char c = released ? 0 : extended_key(sc);
        if (c)
            push(c);
        return;
    }

    if (sc == SC_LSHIFT || sc == SC_RSHIFT) {
        shift = !released;
        return;
    }
    if (released)
        return;
    if (sc == SC_CAPSLOCK) {
        caps = !caps;
        return;
    }
    if (sc == SC_F11) {
        push(KEY_F11);
        return;
    }

    char c = shift ? keymap_shift[sc] : keymap[sc];
    if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    if (c)
        push(c);
}

/*
 * empty out anything the bios left waiting in the keyboard controller, then
 * start listening for keys.
 */
void keyboard_init(void)
{
    while (inb(0x64) & 1)
        inb(KBD_DATA);
    irq_register(1, keyboard_irq);
}

int keyboard_poll(char *c)
{
    if (head == tail)
        return 0;
    *c = buffer[tail];
    tail = (tail + 1) % BUF_SIZE;
    return 1;
}

/*
 * hands back the next key, waiting for one if there isn't any yet. it sleeps
 * until the next interrupt between looks, so other tasks get the cpu in the
 * meantime.
 */
char keyboard_getchar(void)
{
    char c;
    while (!keyboard_poll(&c))
        task_wait();
    return c;
}
