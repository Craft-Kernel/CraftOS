#include <console.h>
#include <io.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((volatile uint16_t *)0xB8000)

#define COM1 0x3F8

static int row, col;
static uint8_t color = VGA_LIGHT_GREY | VGA_BLACK << 4;

/*
 * everything we print is also saved here, so the boot messages can be read
 * again later (they show up as /var/log/boot.log). once it fills up, we just
 * stop saving.
 */
static char log[4096];
static int log_len;

const char *console_log(int *len)
{
    *len = log_len;
    return log;
}

/*
 * the serial port, com1, at 38400 baud. qemu can send whatever we write here
 * to a terminal or a file (with -serial), which is how we test without
 * looking at a screen.
 */

static void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static void serial_putc(char c)
{
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, c);
}

/*
 * vga text mode: the screen is an 80 by 25 grid in memory at 0xb8000, two
 * bytes per cell (the character, then its colours). the blinking cursor is
 * moved by the video card's crt controller, through ports 0x3d4 and 0x3d5.
 */

static void update_cursor(void)
{
    uint16_t pos = row * VGA_WIDTH + col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, pos >> 8);
}

static void scroll(void)
{
    for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++)
        VGA_MEM[i] = VGA_MEM[i + VGA_WIDTH];
    for (int i = 0; i < VGA_WIDTH; i++)
        VGA_MEM[(VGA_HEIGHT - 1) * VGA_WIDTH + i] = ' ' | color << 8;
    row = VGA_HEIGHT - 1;
}

void console_clear(void)
{
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEM[i] = ' ' | color << 8;
    row = col = 0;
    update_cursor();
}

void console_init(void)
{
    serial_init();
    console_clear();
}

void console_set_color(enum vga_color fg, enum vga_color bg)
{
    color = fg | bg << 4;
}

/*
 * puts one character on the screen at the cursor, handling new lines,
 * backspace and tabs, and scrolling everything up when we run off the bottom.
 */
static void vga_putc(char c)
{
    switch (c) {
    case '\n':
        col = 0;
        row++;
        break;
    case '\r':
        col = 0;
        break;
    case '\b':
        if (col > 0) {
            col--;
            VGA_MEM[row * VGA_WIDTH + col] = ' ' | color << 8;
        }
        break;
    case '\t':
        col = (col + 8) & ~7;
        break;
    default:
        VGA_MEM[row * VGA_WIDTH + col] = (uint8_t)c | color << 8;
        col++;
    }
    if (col >= VGA_WIDTH) {
        col = 0;
        row++;
    }
    if (row >= VGA_HEIGHT)
        scroll();
}

/*
 * terminals want \r\n to start a new line, and "\b \b" to actually rub a
 * character out, so the serial copy gets those instead.
 */
void console_putc(char c)
{
    if (log_len < (int)sizeof(log))
        log[log_len++] = c;
    vga_putc(c);
    if (c == '\n') {
        serial_putc('\r');
        serial_putc('\n');
    } else if (c == '\b') {
        serial_putc('\b');
        serial_putc(' ');
        serial_putc('\b');
    } else {
        serial_putc(c);
    }
    update_cursor();
}

void console_write(const char *s)
{
    while (*s)
        console_putc(*s++);
}

/*
 * the actual formatting is done in format.c; this just sends the result to
 * the screen.
 */
void kprintf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_write(buf);
}
