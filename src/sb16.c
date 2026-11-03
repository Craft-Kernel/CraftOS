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
static int sb_present = 0;

#define SINE_AMPLITUDE 30000

/* TODO: generate this at runtime instead of hardcoding 256 sine values. */
/* TODO complete by bitetheapple */
static int16_t sine_table[256];

/* no fpu in the kernel, so this works out a quarter of the wave with whole
   numbers (28-bit fixed point) and sine's taylor series, then mirrors it
   into the other three quarters. */
static void build_sine_table(void)
{
    const int32_t angle_step = 6588397;   /* pi / 128 */

    for (int i = 0; i <= 64; i++) {
        int32_t x = angle_step * i;
        int32_t x2 = (int32_t)(((int64_t)x * x) >> 28);
        int32_t term = x;
        int32_t sum = x;

        for (int k = 1; k <= 6; k++) {
            term = (int32_t)(((int64_t)term * x2) >> 28);
            term /= (2 * k) * (2 * k + 1);
            sum += (k & 1) ? -term : term;
        }

        int16_t v = (int16_t)(((int64_t)sum * SINE_AMPLITUDE + (1 << 27)) >> 28);

        sine_table[i] = v;
        sine_table[128 - i] = v;
        sine_table[128 + i] = -v;
        sine_table[(256 - i) & 0xFF] = -v;
    }
}

static void dsp_write(uint8_t v)
{
    for (int i = 0; i < 100000 && (inb(SB_WRITE) & 0x80); i++) { }
    outb(SB_WRITE, v);
}

static uint8_t dsp_read(void)
{
    for (int i = 0; i < 100000 && !(inb(SB_STATUS) & 0x80); i++) { }
    return inb(SB_READ);
}

static int dsp_reset(void)
{
    outb(SB_RESET, 1);
    for (volatile int i = 0; i < 10000; i++) { }
    outb(SB_RESET, 0);
    for (int i = 0; i < 100000; i++) {
        if (inb(SB_STATUS) & 0x80) {
            if (inb(SB_READ) == 0xAA)
                return 1;
        }
    }
    return 0;
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
    build_sine_table();

    /* no card answered the reset, so leave it alone instead of waiting on it forever */
    if (!dsp_reset())
        return 0;

    /* check version */
    dsp_write(0xE1);
    (void)dsp_read();
    (void)dsp_read();

    irq_register(5, sb16_irq);

    /* enable speaker (sb16 mostly ignores, but harmless) */
    dsp_write(0xD1);

    sb_present = 1;
    return 1;
}

void sb16_play_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!sb_present)
        return;
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
