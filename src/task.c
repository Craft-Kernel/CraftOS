#include <cpu.h>
#include <io.h>
#include <net.h>
#include <string.h>
#include <task.h>

/*
 * the scheduler.
 *
 * a thread that isn't running is parked inside an interrupt: its stack holds
 * the frame isr_common pushed (struct regs), and esp points at it. switching
 * tasks is just a matter of handing back a different task's esp from
 * interrupt_handler, so the pops and the iret carry on as that task instead.
 *
 * a new thread gets a frame built by hand, as if it had been interrupted
 * right at the start of task_start, which calls the real entry point and
 * tidies up if it ever returns.
 *
 * interrupt handlers use the table too, so normal code switches interrupts
 * off (irq_save) while it changes anything.
 */

/*
 * AI Contribution (Bug Fix): stacks are 32 kib now (they were 16), and the
 * bottom word of each one holds STACK_CANARY. the stacks sit right next to
 * each other with nothing in between, so running off the end of one used to
 * quietly scribble over the next thread's saved registers, and something
 * would crash much later for no visible reason. the scheduler checks the
 * canary every time it switches away from a thread, and panics with the
 * thread's name the moment it's been overwritten.
 */
#define STACK_SIZE   32768
#define STACK_CANARY 0xC4AF57ACu

static struct task tasks[MAX_TASKS];
static uint8_t stacks[MAX_TASKS][STACK_SIZE] __attribute__((aligned(16)));

static int current = -1;
static volatile int no_preempt;     /* AI Contribution (Bug Fix): see task_preempt_off */
static int idle_slot;
static int next_pid = 2;
static uint32_t window_start;

/* ---- lookups ---- */

static int slot_of(int pid)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state != TASK_FREE && tasks[i].pid == pid)
            return i;
    return -1;
}

struct task *task_by_pid(int pid)
{
    int s = slot_of(pid);
    return s < 0 ? 0 : &tasks[s];
}

struct task *task_current(void)
{
    return &tasks[current];
}

int task_lookup(const char *s)
{
    if (*s >= '0' && *s <= '9') {
        int pid = 0;
        for (; *s >= '0' && *s <= '9'; s++)
            pid = pid * 10 + (*s - '0');
        return *s || !task_by_pid(pid) ? TASK_ERR_NO_SUCH : pid;
    }
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state != TASK_FREE && tasks[i].state != TASK_DEAD && strcmp(tasks[i].name, s) == 0)
            return tasks[i].pid;
    return TASK_ERR_NO_SUCH;
}

const char *task_error(int err)
{
    switch (err) {
    case TASK_ERR_NO_SUCH:    return "no such task";
    case TASK_ERR_PROTECTED:  return "that's part of the kernel and can't be stopped";
    case TASK_ERR_FULL:       return "too many tasks";
    case TASK_ERR_NOT_THREAD: return "apps can't be paused";
    default:                  return "error";
    }
}

void task_rename(int pid, const char *name)
{
    struct task *t = task_by_pid(pid);
    if (t)
        strlcpy(t->name, name, sizeof(t->name));
}

/* ---- creating and removing ---- */

static int free_slot(void)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_FREE)
            return i;
    return -1;
}

static void task_exit(void)
{
    uint32_t flags = irq_save();
    tasks[current].state = TASK_DEAD;
    irq_restore(flags);
    for (;;)
        task_yield();
}

static void task_start(void)
{
    tasks[current].entry();
    task_exit();
}

/*
 * builds the pretend interrupt frame for a new task. from the top of its
 * stack down: a dummy return address for task_start, then exactly what the
 * interrupt stub would have left behind, in the order it pops it back off.
 * the saved eflags (0x202) has interrupts switched on.
 */
static int make_thread(int slot, int pid, const char *name, void (*entry)(void))
{
    *(uint32_t *)stacks[slot] = STACK_CANARY;
    uint32_t *sp = (uint32_t *)(stacks[slot] + STACK_SIZE);
    *--sp = 0;
    *--sp = 0x202;
    *--sp = 0x08;
    *--sp = (uint32_t)task_start;
    *--sp = 0;
    *--sp = 0;
    for (int i = 0; i < 8; i++)
        *--sp = 0;
    for (int i = 0; i < 4; i++)
        *--sp = 0x10;

    struct task *t = &tasks[slot];
    memset(t, 0, sizeof(*t));
    t->pid = pid;
    t->kind = TASK_THREAD;
    t->esp = (uint32_t)sp;
    t->entry = entry;
    t->started = timer_ticks();
    strlcpy(t->name, name, sizeof(t->name));
    t->state = TASK_READY;
    return pid;
}

int task_spawn(const char *name, void (*entry)(void))
{
    uint32_t flags = irq_save();
    int slot = free_slot();
    int pid = slot < 0 ? TASK_ERR_FULL : make_thread(slot, next_pid++, name, entry);
    irq_restore(flags);
    return pid;
}

int task_add_app(const char *name, void (*close)(void *ctx), void *ctx)
{
    uint32_t flags = irq_save();
    int slot = free_slot();
    int pid = TASK_ERR_FULL;
    if (slot >= 0) {
        struct task *t = &tasks[slot];
        memset(t, 0, sizeof(*t));
        t->pid = pid = next_pid++;
        t->kind = TASK_APP;
        t->close = close;
        t->ctx = ctx;
        t->started = timer_ticks();
        strlcpy(t->name, name, sizeof(t->name));
        t->state = TASK_READY;
    }
    irq_restore(flags);
    return pid;
}

void task_remove(int pid)
{
    uint32_t flags = irq_save();
    int s = slot_of(pid);
    if (s >= 0 && tasks[s].kind == TASK_APP)
        tasks[s].state = TASK_FREE;
    irq_restore(flags);
}

/* ---- kill, pause, resume ---- */

bool task_is_kernel(int pid)
{
    return pid == 0 || pid == 1;
}

/*
 * killing a kernel task is allowed, but only on purpose: task_kill says no,
 * and the os asks the user first before calling this one instead. whatever
 * happens next gets dealt with in task_schedule.
 */
int task_kill_anyway(int pid)
{
    int s = slot_of(pid);
    if (s < 0 || tasks[s].state == TASK_DEAD)
        return TASK_ERR_NO_SUCH;

    uint32_t flags = irq_save();
    tasks[s].state = TASK_DEAD;
    irq_restore(flags);
    tcp_drop_task(pid);     /* AI Contribution (Bug Fix): give back its sockets */

    if (s == current)
        task_yield();
    return 0;
}

/*
 * ends a task. the kernel's own tasks are protected. an app's task gets
 * closed through its window, and a thread just gets marked dead, so the
 * scheduler can give its slot back once it's off the cpu.
 */
int task_kill(int pid)
{
    if (task_is_kernel(pid))
        return TASK_ERR_PROTECTED;
    int s = slot_of(pid);
    if (s < 0 || tasks[s].state == TASK_DEAD)
        return TASK_ERR_NO_SUCH;

    /*
     * an app's task is closed through its window, which removes the task
     * itself
     */
    if (tasks[s].kind == TASK_APP) {
        tasks[s].close(tasks[s].ctx);
        task_remove(pid);
        return 0;
    }

    uint32_t flags = irq_save();
    tasks[s].state = TASK_DEAD;
    irq_restore(flags);
    tcp_drop_task(pid);     /* AI Contribution (Bug Fix): give back its sockets */
    return 0;
}

static int set_paused(int pid, bool pause)
{
    if (task_is_kernel(pid))
        return TASK_ERR_PROTECTED;
    int s = slot_of(pid);
    if (s < 0 || tasks[s].state == TASK_DEAD)
        return TASK_ERR_NO_SUCH;
    if (tasks[s].kind != TASK_THREAD)
        return TASK_ERR_NOT_THREAD;

    uint32_t flags = irq_save();
    if (pause)
        tasks[s].state = TASK_PAUSED;
    else if (tasks[s].state == TASK_PAUSED)
        tasks[s].state = TASK_READY;
    irq_restore(flags);
    return 0;
}

int task_pause(int pid)
{
    return set_paused(pid, true);
}

int task_resume(int pid)
{
    return set_paused(pid, false);
}

/* ---- listing ---- */

int task_snapshot(struct task **out, int max)
{
    int n = 0;
    for (int i = 0; i < MAX_TASKS && n < max; i++) {
        if (tasks[i].state == TASK_FREE || tasks[i].state == TASK_DEAD)
            continue;
        int j = n++;
        while (j > 0 && out[j - 1]->pid > tasks[i].pid) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = &tasks[i];
    }
    return n;
}

const char *task_state_name(const struct task *t)
{
    if (t->kind == TASK_APP)
        return "app";
    if (t == &tasks[current])
        return "running";
    switch (t->state) {
    case TASK_READY:    return "ready";
    case TASK_WAITING:  return "waiting";
    case TASK_SLEEPING: return "sleeping";
    case TASK_PAUSED:   return "paused";
    default:            return "?";
    }
}

/*
 * writes out the task list the way ps shows it: pid, state, cpu use, time
 * used, name and whatever status line the task keeps for itself. apps don't
 * have cpu numbers of their own, since they run inside the desktop.
 */
int task_describe(char *buf, int size)
{
    struct task *list[MAX_TASKS];
    int n = task_snapshot(list, MAX_TASKS);
    uint32_t hz = timer_hz();

    int len = ksnprintf(buf, size, "  PID  STATE     CPU   TIME  NAME        INFO\n");
    for (int i = 0; i < n && len < size; i++) {
        struct task *t = list[i];
        if (t->kind == TASK_APP) {
            len += ksnprintf(buf + len, size - len, "%5d  %-8s    -      -  %-10s  %s\n",
                             t->pid, task_state_name(t), t->name, t->info);
        } else {
            uint32_t secs = t->ticks / hz;
            len += ksnprintf(buf + len, size - len, "%5d  %-8s %3u%%  %2u:%02u  %-10s  %s\n",
                             t->pid, task_state_name(t), t->cpu_percent,
                             secs / 60, secs % 60, t->name, t->info);
        }
    }
    return len < size ? len : size - 1;
}

/*
 * AI Contribution (Bug Fix): switches preemption off and back on, counting,
 * so it can be nested. while it's off, the timer doesn't take the cpu away
 * from whoever's running, but interrupts still happen (the clock, the
 * keyboard and the network keep working). it's how the ram disk keeps one
 * thread from being switched out halfway through changing it.
 */
void task_preempt_off(void)
{
    __atomic_add_fetch(&no_preempt, 1, __ATOMIC_SEQ_CST);
}

void task_preempt_on(void)
{
    __atomic_sub_fetch(&no_preempt, 1, __ATOMIC_SEQ_CST);
}

/* ---- giving up the cpu ---- */

void task_yield(void)
{
    if (current >= 0)
        __asm__ volatile("int %0" : : "i"(TASK_YIELD_VECTOR));
    else
        __asm__ volatile("hlt");
}

void task_wait(void)
{
    if (current < 0) {
        __asm__ volatile("hlt");
        return;
    }
    uint32_t flags = irq_save();
    tasks[current].state = TASK_WAITING;
    irq_restore(flags);
    task_yield();
}

void task_sleep(uint32_t ticks)
{
    uint32_t flags = irq_save();
    tasks[current].state = TASK_SLEEPING;
    tasks[current].wake_tick = timer_ticks() + ticks;
    irq_restore(flags);
    task_yield();
}

void task_wake_waiting(void)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_WAITING)
            tasks[i].state = TASK_READY;
}

/* ---- the scheduler itself ---- */

/*
 * once a second, each task's tick count becomes a cpu percentage. a task gets
 * charged for every timer tick that lands while it's the one running, so
 * whatever the idle task ends up with is the time nobody else wanted.
 */
static void account(uint32_t now)
{
    tasks[current].ticks++;
    tasks[current].window_ticks++;

    uint32_t span = now - window_start;
    if (span < timer_hz())
        return;
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].cpu_percent = tasks[i].window_ticks * 100 / span;
        tasks[i].window_ticks = 0;
    }
    window_start = now;
}

/*
 * picks who runs next, round robin: starting just after the current task,
 * take the first one that's ready. the idle task only gets a go when nothing
 * else is ready, and dead tasks hand their slot back once they're safely off
 * the cpu.
 *
 * if someone insisted on killing a kernel task, this is where it catches up
 * with them. without pid 1 (slot 0) the shell and desktop are gone, so the
 * kernel panics. without the idle task, there's nothing to fall back on when
 * everyone else is waiting, so the cpu just stays with whoever had it and
 * spins instead of resting.
 */
uint32_t task_schedule(uint32_t esp, bool tick)
{
    if (current < 0)
        return esp;

    uint32_t now = timer_ticks();
    tasks[current].esp = esp;
    if (tick)
        account(now);

    /*
     * AI Contribution (Bug Fix): slot 0 runs on the boot stack, so it has no
     * canary of its own to check
     */
    if (current != 0 && tasks[current].kind == TASK_THREAD &&
        *(uint32_t *)stacks[current] != STACK_CANARY) {
        static char why[64];
        ksnprintf(why, sizeof(why), "task %d (%s) ran out of stack", tasks[current].pid, tasks[current].name);
        panic(why);
    }

    /*
     * AI Contribution (Bug Fix): while something has switched preemption off
     * (the ram disk, mostly), a timer tick doesn't move the cpu on. a task
     * giving up its turn on purpose still does.
     */
    if (tick && no_preempt)
        return esp;

    if (tasks[0].state == TASK_DEAD)
        panic("the kernel task (pid 1) was killed");

    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_SLEEPING && (int32_t)(now - t->wake_tick) >= 0)
            t->state = TASK_READY;
        if (t->state == TASK_DEAD && i != current)
            t->state = TASK_FREE;
    }

    int next = -1;
    for (int i = 1; i <= MAX_TASKS; i++) {
        int s = (current + i) % MAX_TASKS;
        if (s != idle_slot && tasks[s].kind == TASK_THREAD && tasks[s].state == TASK_READY) {
            next = s;
            break;
        }
    }

    if (next < 0) {
        bool idle_alive = tasks[idle_slot].pid == 0 && tasks[idle_slot].kind == TASK_THREAD &&
                          tasks[idle_slot].state != TASK_FREE && tasks[idle_slot].state != TASK_DEAD;
        if (idle_alive)
            next = idle_slot;
        else if (tasks[current].state != TASK_DEAD && tasks[current].state != TASK_FREE)
            next = current;
        else
            panic("no tasks left to run");
    }

    current = next;
    return tasks[next].esp;
}

static void idle_main(void)
{
    for (;;) {
        __asm__ volatile("hlt");
        task_yield();
    }
}

/*
 * the code that's already running (kernel_main, still on the boot stack)
 * becomes pid 1 in slot 0. we don't know its esp yet; that gets saved the
 * first time the scheduler switches away from it.
 */
void tasks_init(void)
{
    struct task *k = &tasks[0];
    k->pid = 1;
    k->kind = TASK_THREAD;
    k->state = TASK_READY;
    strlcpy(k->name, "shell", sizeof(k->name));

    idle_slot = 1;
    make_thread(idle_slot, 0, "idle", idle_main);

    window_start = timer_ticks();
    current = 0;
}
