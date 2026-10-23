#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * everything the interrupt stub (isr_common) saves on the stack, in memory
 * order. the last three get pushed by the cpu itself before the stub even
 * starts.
 */
struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
};

typedef void (*irq_handler_t)(struct regs *r);

void gdt_init(void);
void idt_init(void);

void pic_init(void);
void pic_unmask(uint8_t irq);
void pic_send_eoi(uint8_t irq);
void irq_register(uint8_t irq, irq_handler_t handler);

void timer_init(uint32_t hz);
uint32_t timer_ticks(void);
uint32_t timer_hz(void);

/*
 * keyboard_getchar waits for a key; keyboard_poll gives up straight away and
 * returns 0 if there isn't one. keys come through as ordinary characters,
 * except for these, which borrow control codes nobody can type.
 */
#define KEY_UP   0x11
#define KEY_DOWN 0x12
#define KEY_PGUP 0x13
#define KEY_PGDN 0x14
#define KEY_F11  0x15
#define KEY_CTRL_ALT_DEL 0x16

void keyboard_init(void);
char keyboard_getchar(void);
int keyboard_poll(char *c);

/*
 * the mouse keeps its own position, somewhere inside the width and height
 * given to mouse_init. wheel is how far the wheel has turned since you last
 * asked (towards you counts as positive), and it's always 0 on a mouse that
 * doesn't have one.
 */
void mouse_init(int width, int height);
bool mouse_has_wheel(void);
void mouse_get(int *x, int *y, uint8_t *buttons, int *wheel);

/* the same, but leaves the wheel movement there for the next mouse_get. */
void mouse_peek(int *x, int *y, uint8_t *buttons, int *wheel);

void rtc_time(int *hour, int *minute, int *second);
void rtc_date(int *year, int *month, int *day);

/* machine_poweroff only comes back if the machine didn't actually turn off. */
void machine_reboot(void) __attribute__((noreturn));
void machine_poweroff(void);

/* how much memory there is above 1 mib, as the bootloader told us. */
uint32_t system_mem_kib(void);

void panic(const char *msg) __attribute__((noreturn));
