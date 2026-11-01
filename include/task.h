#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * tasks: everything that shows up in ps and the task manager.
 *
 * most of them are kernel threads. each one has its own stack, and the timer
 * swaps between the ones that are ready 100 times a second, taking turns
 * (preemptive round robin). pid 1 is the kernel's own thread, which runs the
 * shell and, once you type `ui`, the desktop. pid 0 is the idle thread, which
 * only gets a turn when nobody else can go, and just rests the cpu until the
 * next interrupt.
 *
 * desktop apps are tasks too, but not threads: they all run inside the
 * desktop's thread, and they're only listed so you can see them. killing one
 * closes its window.
 */

#define MAX_TASKS          16
#define TASK_YIELD_VECTOR  48

enum task_state {
    TASK_FREE,
    TASK_READY,
    TASK_WAITING,
    TASK_SLEEPING,
    TASK_PAUSED,
    TASK_DEAD,
};

enum task_kind { TASK_THREAD, TASK_APP };

/* errors are negative numbers, the same as the file system's. */
enum {
    TASK_ERR_NO_SUCH = -1,
    TASK_ERR_PROTECTED = -2,
    TASK_ERR_FULL = -3,
    TASK_ERR_NOT_THREAD = -4,
};

struct task {
    int pid;
    enum task_state state;
    enum task_kind kind;
    char name[16];
    char info[48];
    uint32_t started;

    /*
     * for threads: the saved stack, when to wake up, and how much cpu they've
     * used
     */
    uint32_t esp;
    uint32_t wake_tick;
    uint32_t ticks;
    uint32_t window_ticks;
    uint32_t cpu_percent;
    void (*entry)(void);

    /* for apps: how to close them */
    void (*close)(void *ctx);
    void *ctx;
};

void tasks_init(void);

int task_spawn(const char *name, void (*entry)(void));
int task_add_app(const char *name, void (*close)(void *ctx), void *ctx);
void task_remove(int pid);

/*
 * task_kill won't touch the kernel tasks (pid 0 and 1). task_kill_anyway
 * will: it's for after the user has been warned and said yes anyway. killing
 * pid 1 panics the kernel; killing pid 0 leaves the cpu spinning whenever it
 * has nothing to do.
 */
bool task_is_kernel(int pid);
int task_kill(int pid);
int task_kill_anyway(int pid);
int task_pause(int pid);
int task_resume(int pid);
const char *task_error(int err);

/* finds a task by its pid, or by name if what you gave it isn't a number. */
int task_lookup(const char *pid_or_name);

struct task *task_by_pid(int pid);
struct task *task_current(void);
void task_rename(int pid, const char *name);

/* fills out[] with the tasks in pid order, and says how many there were. */
int task_snapshot(struct task **out, int max);

const char *task_state_name(const struct task *t);

/* writes out the ps table, for the shell and /proc/tasks. */
int task_describe(char *buf, int size);

/*
 * ways to give up the cpu. task_wait sleeps until the next interrupt of any
 * kind (a key, the mouse, the timer), which is what the shell and the desktop
 * do while they wait for something to happen. task_sleep sleeps for a set
 * number of ticks.
 */
void task_yield(void);
void task_wait(void);
void task_sleep(uint32_t ticks);

/*
 * AI Contribution (Bug Fix): keeps the timer from switching tasks until the
 * matching task_preempt_on. they nest. don't sleep or wait in between.
 */
void task_preempt_off(void);
void task_preempt_on(void);

/* these get called from interrupt_handler. */
uint32_t task_schedule(uint32_t esp, bool tick);
void task_wake_waiting(void);
