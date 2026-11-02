#include <console.h>
#include <cpu.h>
#include <io.h>
#include <sb16.h>
#include <task.h>

#define SB_BASE      0x220
#define SB_RESET     (SB_BASE + 6)
#define SB_READ      (SB_BASE + 0x0A)
#define SB_WRITE     (SB_BASE + 0x0C)
#define SB_STATUS    (SB_BASE + 0x0E)
#define SB_ACK16     (SB_BASE + 0x0F)

#define SAMPLE_RATE  22050
#define MAX_SAMPLES  22050   /* ~1 second at 22050 hz */

static int16_t audio_buf[MAX_SAMPLES] __attribute__((aligned(131072)));
static volatile int sb_done = 0;

/* TODO: generate this at runtime instead of hardcoding 256 sine values. */
static const int16_t sine_table[256] = {
0, 736, 1472, 2206, 2940, 3672, 4401, 5128, 5852, 6573, 7289, 8001, 8708, 9410, 10106, 10796, 11480, 12157, 12826, 13488, 14141, 14786, 15423, 16049, 16667, 17274, 17870, 18456, 19031, 19595, 20146, 20686, 21213, 21727, 22228, 22716, 23190, 23650, 24096, 24527, 24944, 25345, 25731, 26102, 26457, 26796, 27119, 27426, 27716, 27989, 28246, 28485, 28708, 28913, 29100, 29271, 29423, 29558, 29675, 29774, 29855, 29918, 29963, 29990, 30000, 29990, 29963, 29918, 29855, 29774, 29675, 29558, 29423, 29271, 29100, 28913, 28708, 28485, 28246, 27989, 27716, 27426, 27119, 26796, 26457, 26102, 25731, 25345, 24944, 24527, 24096, 23650, 23190, 22716, 22228, 21727, 21213, 20686, 20146, 19595, 19031, 18456, 17870, 17274, 16667, 16049, 15423, 14786, 14141, 13488, 12826, 12157, 11480, 10796, 10106, 9410, 8708, 8001, 7289, 6573, 5852, 5128, 4401, 3672, 2940, 2206, 1472, 736, 0, -736, -1472, -2206, -2940, -3672, -4401, -5128, -5852, -6573, -7289, -8001, -8708, -9410, -10106, -10796, -11480, -12157, -12826, -13488, -14141, -14786, -15423, -16049, -16667, -17274, -17870, -18456, -19031, -19595, -20146, -20686, -21213, -21727, -22228, -22716, -23190, -23650, -24096, -24527, -24944, -25345, -25731, -26102, -26457, -26796, -27119, -27426, -27716, -27989, -28246, -28485, -28708, -28913, -29100, -29271, -29423, -29558, -29675, -29774, -29855, -29918, -29963, -29990, -30000, -29990, -29963, -29918, -29855, -29774, -29675, -29558, -29423, -29271, -29100, -28913, -28708, -28485, -28246, -27989, -27716, -27426, -27119, -26796, -26457, -26102, -25731, -25345, -24944, -24527, -24096, -23650, -23190, -22716, -22228, -21727, -21213, -20686, -20146, -19595, -19031, -18456, -17870, -17274, -16667, -16049, -15423, -14786, -14141, -13488, -12826, -12157, -11480, -10796, -10106, -9410, -8708, -8001, -7289, -6573, -5852, -5128, -4401, -3672, -2940, -2206, -1472, -736
};

static void dsp_write(uint8_t v)
{
    while (inb(SB_WRITE) & 0x80) { }
    outb(SB_WRITE, v);
}

static uint8_t dsp_read(void)
{
    while (!(inb(SB_STATUS) & 0x80)) { }
    return inb(SB_READ);
}

static void dsp_reset(void)
{
    outb(SB_RESET, 1);
    for (volatile int i = 0; i < 10000; i++) { }
    outb(SB_RESET, 0);
    for (int i = 0; i < 100000; i++) {
        if (inb(SB_STATUS) & 0x80) {
            if (inb(SB_READ) == 0xAA)
                return;
        }
    }
}

static void sb16_irq(struct regs *r)
{
    (void)r;
    inb(SB_ACK16);
    sb_done = 1;
    pic_send_eoi(5);
}

static void dma_setup(uint32_t samples)
{
    uint32_t bytes = samples * 2;
    uint32_t words = samples;
    uint32_t addr = (uint32_t)audio_buf;

    /* mask channel 5 (relative channel 1 on the secondary controller) */
    outb(0xD4, (5 & 3) | 4);
    /* reset flip-flop */
    outb(0xD8, 0x00);
    /* address (word address) */
    uint32_t wa = addr >> 1;
    outb(0xC4, wa & 0xFF);
    outb(0xC4, (wa >> 8) & 0xFF);
    outb(0xD8, 0x00);
    /* count (words - 1) */
    uint32_t cnt = words - 1;
    outb(0xC6, cnt & 0xFF);
    outb(0xC6, (cnt >> 8) & 0xFF);
    /* page */
    outb(0x8B, (addr >> 16) & 0xFF);
    /* mode: single, read from memory, channel 5 */
    outb(0xD6, 0x48 | (5 & 3));
    /* unmask */
    outb(0xD4, (5 & 3));
    (void)bytes;
}

static void sb16_play(uint32_t samples)
{
    if (samples == 0 || samples > MAX_SAMPLES)
        samples = MAX_SAMPLES;

    sb_done = 0;

    /* set sample rate */
    dsp_write(0x41);
    dsp_write((SAMPLE_RATE >> 8) & 0xFF);
    dsp_write(SAMPLE_RATE & 0xFF);

    dma_setup(samples);

    /* 16-bit DAC, single-cycle, signed mono */
    dsp_write(0xB0);
    dsp_write(0x08);
    uint32_t len = samples * 2 - 1;
    dsp_write(len & 0xFF);
    dsp_write((len >> 8) & 0xFF);

    /* wait for completion (with a timeout so a missing irq can't hang the kernel) */
    uint32_t start = timer_ticks();
    while (!sb_done && (int32_t)(timer_ticks() - start) < (int32_t)(timer_hz() * 3)) {
        task_yield();
    }
}

int sb16_init(void)
{
    dsp_reset();

    /* check version */
    dsp_write(0xE1);
    (void)dsp_read();
    (void)dsp_read();

    irq_register(5, sb16_irq);

    /* enable speaker (sb16 mostly ignores, but harmless) */
    dsp_write(0xD1);

    return 1;
}

void sb16_play_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (freq_hz == 0)
        freq_hz = 440;
    if (duration_ms == 0)
        duration_ms = 500;
    if (duration_ms > 1000)
        duration_ms = 1000;

    uint32_t samples = (SAMPLE_RATE * duration_ms) / 1000;
    if (samples == 0)
        samples = 1;
    if (samples > MAX_SAMPLES)
        samples = MAX_SAMPLES;

    uint32_t phase = 0;
    uint32_t step = (freq_hz * 256) / SAMPLE_RATE;
    if (step == 0)
        step = 1;
    for (uint32_t i = 0; i < samples; i++) {
        audio_buf[i] = sine_table[phase & 0xFF];
        phase += step;
    }

    sb16_play(samples);
}