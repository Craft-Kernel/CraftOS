#pragma once
#include <stdint.h>

/*
 * talking to hardware through x86 i/o ports, plus a couple of tiny helpers
 * that get used all over the kernel.
 */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/*
 * old interrupt controllers need a moment to catch their breath between
 * commands. writing to port 0x80 (which nothing uses any more; it used to
 * show boot progress codes) takes about a microsecond, which is just enough.
 */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

/*
 * switching interrupts off is how the kernel stops the scheduler from
 * swapping tasks in the middle of something delicate. irq_save remembers
 * whether they were on to begin with, so irq_restore can put things back
 * exactly how they were.
 */
static inline uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    __asm__ volatile("push %0; popf" : : "r"(flags) : "memory", "cc");
}

__attribute__((noreturn)) static inline void halt_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}
