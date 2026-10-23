#include <cpu.h>
#include <fs.h>
#include <io.h>
#include <programs.h>
#include <string.h>
#include <task.h>

/*
 * the background programs you can start from the shell. none of them ever
 * finish: they keep going until somebody kills them. each one keeps a short
 * status line in its task's info field, which ps and the task manager show.
 */

/*
 * spin does nothing but count as fast as it can, so you can watch it eat the
 * cpu.
 */
static void spin_main(void)
{
    struct task *self = task_current();
    uint32_t rounds = 0;
    for (;;) {
        for (volatile int i = 0; i < 100000; i++)
            ;
        rounds++;
        ksnprintf(self->info, sizeof(self->info), "%u00k loops", rounds);
    }
}

/* primes checks every number in turn by trying to divide it, forever. */
static void primes_main(void)
{
    struct task *self = task_current();
    uint32_t found = 0;
    for (uint32_t n = 2;; n++) {
        bool prime = true;
        for (uint32_t d = 2; d * d <= n; d++) {
            if (n % d == 0) {
                prime = false;
                break;
            }
        }
        if (prime) {
            found++;
            ksnprintf(self->info, sizeof(self->info), "%u found, latest %u", found, n);
        }
    }
}

/*
 * ticker sleeps nearly all the time, so it hardly uses any cpu. once a second
 * it wakes up and adds a line to /tmp/ticker.log, starting the file over when
 * it gets full. the ram disk isn't built for two tasks writing at once, so
 * the write happens with interrupts switched off.
 */
static void ticker_main(void)
{
    struct task *self = task_current();
    for (uint32_t count = 1;; count++) {
        task_sleep(timer_hz());

        int h, m, s;
        rtc_time(&h, &m, &s);
        char line[48];
        int len = ksnprintf(line, sizeof(line), "tick %u at %02u:%02u:%02u\n", count, h, m, s);

        uint32_t flags = irq_save();
        int f = fs_resolve(FS_ROOT, "/tmp/ticker.log");
        if (f < 0)
            f = fs_create(FS_ROOT, "/tmp/ticker.log", false);
        if (f >= 0) {
            bool full = fs_node(f)->size + len > FS_FILE_MAX;
            fs_write(f, line, len, !full);
        }
        irq_restore(flags);

        ksnprintf(self->info, sizeof(self->info), "%u ticks, see /tmp/ticker.log", count);
    }
}

const struct program programs[] = {
    { "spin", "spin  (program) count as fast as possible, to watch cpu use", spin_main },
    { "primes", "primes  (program) search for prime numbers", primes_main },
    { "ticker", "ticker  (program) log the time to /tmp/ticker.log every second", ticker_main },
};

const int program_count = sizeof(programs) / sizeof(programs[0]);

const struct program *program_find(const char *name)
{
    for (int i = 0; i < program_count; i++)
        if (strcmp(programs[i].name, name) == 0)
            return &programs[i];
    return 0;
}
