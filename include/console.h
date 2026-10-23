#pragma once
#include <stdint.h>

enum vga_color {
    VGA_BLACK, VGA_BLUE, VGA_GREEN, VGA_CYAN, VGA_RED, VGA_MAGENTA, VGA_BROWN,
    VGA_LIGHT_GREY, VGA_DARK_GREY, VGA_LIGHT_BLUE, VGA_LIGHT_GREEN,
    VGA_LIGHT_CYAN, VGA_LIGHT_RED, VGA_LIGHT_MAGENTA, VGA_YELLOW, VGA_WHITE,
};

/*
 * the text screen. everything printed here also gets copied to the com1
 * serial port, so you can read the output without looking at a screen.
 */
void console_init(void);
void console_clear(void);
void console_set_color(enum vga_color fg, enum vga_color bg);
void console_putc(char c);
void console_write(const char *s);

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * everything printed so far (the first 4 kib, anyway), for /var/log/boot.log.
 */
const char *console_log(int *len);
