#include <cpu.h>
#include <io.h>

/*
 * the timer chip (the old 8253/8254 pit) counts down from a number we give
 * it, 1,193,182 times a second, and taps the cpu on the shoulder (irq 0)
 * every time it hits zero. so if we tell it to count from 1193182 / hz, we
 * get hz taps a second. that steady beat is the only clock the kernel keeps
 * right now: everything that needs to know how long it's been just counts the
 * ticks.
 */

#define PIT_BASE_HZ 1193182

static volatile uint32_t ticks;
static uint32_t frequency;

static void timer_irq(struct regs *r)
{
    (void)r;
    ticks++;
}

void timer_init(uint32_t hz)
{
    uint32_t divisor = PIT_BASE_HZ / hz;
    frequency = hz;

    /*
     * set channel 0 to repeat forever (square wave mode), then hand it the
     * starting count one byte at a time, low byte first.
     */
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);

    irq_register(0, timer_irq);
}

uint32_t timer_ticks(void)
{
    return ticks;
}

uint32_t timer_hz(void)
{
    return frequency;
}
