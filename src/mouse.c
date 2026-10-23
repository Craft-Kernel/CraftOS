#include <cpu.h>
#include <io.h>

/*
 * the ps/2 mouse, on irq 12. it lives on the same controller chip as the
 * keyboard, so to talk to it we send 0xd4 to port 0x64 first, which means
 * "pass the next byte on to the mouse".
 *
 * an ordinary mouse sends little 3-byte packets: one with the buttons and the
 * sign bits, one with how far it moved sideways, and one with how far it
 * moved up or down (up counts as positive). we add those moves up into a
 * position on the screen, and keep it from wandering off the edges.
 *
 * wheel mice pretend to be ordinary ones until you know the secret knock: set
 * the sample rate to 200, then 100, then 80. after that the mouse says it's
 * type 3 and adds a fourth byte to every packet with the wheel's movement.
 */

#define KBC_DATA   0x60
#define KBC_STATUS 0x64

static volatile int mouse_x, mouse_y, mouse_wheel;
static volatile uint8_t mouse_buttons;
static int max_x, max_y;

static int packet_size = 3;
static uint8_t packet[4];
static int packet_len;

/*
 * the controller can be slow to get ready, but if it never does we'd rather
 * give up after a while than hang the whole machine.
 */
static void wait_write(void)
{
    for (int i = 0; i < 100000 && (inb(KBC_STATUS) & 2); i++)
        ;
}

static void wait_read(void)
{
    for (int i = 0; i < 100000 && !(inb(KBC_STATUS) & 1); i++)
        ;
}

static uint8_t mouse_command(uint8_t cmd)
{
    wait_write();
    outb(KBC_STATUS, 0xD4);
    wait_write();
    outb(KBC_DATA, cmd);
    wait_read();
    return inb(KBC_DATA);
}

static void set_sample_rate(uint8_t rate)
{
    mouse_command(0xF3);
    mouse_command(rate);
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/*
 * runs for every byte the mouse sends. bytes are collected until there's a
 * whole packet, which then gets turned into a new position, the buttons held
 * down, and how far the wheel turned.
 */
static void mouse_irq(struct regs *r)
{
    (void)r;
    uint8_t byte = inb(KBC_DATA);

    /*
     * the first byte of a real packet always has bit 3 set, and (in practice)
     * never the overflow bits, 6 and 7. if a byte doesn't look like that,
     * we've lost track of where packets start, so we throw bytes away until
     * one fits. checking the overflow bits matters more than it looks: a
     * stray 0xfa acknowledgement has bit 3 set too, and so does every byte of
     * some ordinary movements, so bit 3 on its own can stay confused for a
     * long time.
     */
    if (packet_len == 0 && (!(byte & 0x08) || (byte & 0xC0)))
        return;
    packet[packet_len++] = byte;
    if (packet_len < packet_size)
        return;
    packet_len = 0;

    int dx = packet[1] - ((packet[0] << 4) & 0x100);
    int dy = packet[2] - ((packet[0] << 3) & 0x100);
    mouse_x = clamp(mouse_x + dx, 0, max_x);
    mouse_y = clamp(mouse_y - dy, 0, max_y);
    mouse_buttons = packet[0] & 0x07;
    if (packet_size == 4)
        mouse_wheel += (int8_t)packet[3];
}

/*
 * switches on the controller's second port (the one the mouse uses), lets it
 * raise irq 12, resets the mouse to its defaults, tries the secret knock for
 * wheel mode, and tells it to start sending.
 */
void mouse_init(int width, int height)
{
    max_x = width - 1;
    max_y = height - 1;
    mouse_x = width / 2;
    mouse_y = height / 2;

    wait_write();
    outb(KBC_STATUS, 0xA8);

    wait_write();
    outb(KBC_STATUS, 0x20);
    wait_read();
    uint8_t config = inb(KBC_DATA);
    config |= 0x02;
    config &= ~0x20;
    wait_write();
    outb(KBC_STATUS, 0x60);
    wait_write();
    outb(KBC_DATA, config);

    mouse_command(0xF6);

    set_sample_rate(200);
    set_sample_rate(100);
    set_sample_rate(80);
    mouse_command(0xF2);
    wait_read();
    if (inb(KBC_DATA) == 3)
        packet_size = 4;

    mouse_command(0xF4);

    /*
     * an acknowledgement that arrived late might still be sitting in the
     * controller. clear it out so it doesn't get mistaken for the start of a
     * packet.
     */
    while (inb(KBC_STATUS) & 1)
        inb(KBC_DATA);

    irq_register(12, mouse_irq);
}

bool mouse_has_wheel(void)
{
    return packet_size == 4;
}

/*
 * interrupts pause for a moment while we copy everything, so the position,
 * buttons and wheel all come from the same packet.
 */
void mouse_get(int *x, int *y, uint8_t *buttons, int *wheel)
{
    uint32_t flags = irq_save();
    *x = mouse_x;
    *y = mouse_y;
    *buttons = mouse_buttons;
    *wheel = mouse_wheel;
    mouse_wheel = 0;
    irq_restore(flags);
}

void mouse_peek(int *x, int *y, uint8_t *buttons, int *wheel)
{
    uint32_t flags = irq_save();
    *x = mouse_x;
    *y = mouse_y;
    *buttons = mouse_buttons;
    *wheel = mouse_wheel;
    irq_restore(flags);
}
