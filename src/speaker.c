#include <cpu.h>
#include <io.h>
#include <speaker.h>

#define PIT_BASE_HZ 1193182

void speaker_off(void)
{
    uint8_t p = inb(0x61);
    outb(0x61, p & ~3);
}

void speaker_beep(uint32_t freq_hz, uint32_t duration_ms)
{
    if (freq_hz == 0)
        return;

    uint32_t divisor = PIT_BASE_HZ / freq_hz;
    if (divisor == 0)
        divisor = 1;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;

    /* channel 2, lobyte/hibyte, square wave */
    outb(0x43, 0xB6);
    outb(0x42, divisor & 0xFF);
    outb(0x42, (divisor >> 8) & 0xFF);

    uint8_t p = inb(0x61);
    outb(0x61, p | 3);

    if (duration_ms > 0) {
        uint32_t ticks = timer_ticks();
        uint32_t hz = timer_hz();
        uint32_t target = ticks + (duration_ms * hz + 999) / 1000;
        if (target == ticks)
            target = ticks + 1;
        while (timer_ticks() < target) {
            /* wait for the timer to advance; interrupts stay on */
        }
    }

    speaker_off();
}