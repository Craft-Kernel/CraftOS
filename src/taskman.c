#include <apps.h>
#include <cpu.h>
#include <gfx.h>
#include <programs.h>
#include <string.h>
#include <task.h>

/*
 * the task manager. every task goes in a table (its pid, name, state, how
 * much of the cpu it's using, how much cpu time it's had, and its status
 * line), which refreshes once a second along with the rest of the desktop.
 * underneath are buttons to end, pause or resume whichever task is selected,
 * and one to start each of the background programs.
 *
 * ctrl+alt+del opens it from anywhere on the desktop.
 */

#define HEADER_H 18
#define ROW_H    18
#define BUTTON_H 22
#define STATUS_H 20
#define BOTTOM_H (BUTTON_H + 6 + STATUS_H)

static int selected_pid = -1;
static char status[64];
static struct scrollbar sb = { .grab = -1 };

static void draw(struct window *win, int cx, int cy, int cw, int ch);
static bool key(struct window *win, char c);
static bool click(struct window *win, int x, int y, bool double_click);
static bool drag(struct window *win, int x, int y);
static bool scroll(struct window *win, int rows);

struct window taskman_window = {
    .title = "Task Manager", .proc = "taskman", .x = 110, .y = 50, .w = 500, .h = 320,
    .min_w = 490, .min_h = 200, .resizable = true,
    .draw = draw, .key = key, .click = click, .drag = drag, .scroll = scroll,
};

static const struct {
    const char *label;
    int x;
} columns[] = {
    { "PID", 0 }, { "Name", 44 }, { "State", 144 }, { "CPU", 224 }, { "Time", 270 }, { "Info", 324 },
};

#define COLUMN_COUNT ((int)(sizeof(columns) / sizeof(columns[0])))

enum { BTN_END, BTN_PAUSE, BTN_RESUME, BTN_FIRST_PROGRAM };

static const struct {
    const char *label;
    int x, w;
} buttons[] = {
    [BTN_END] = { "End Task", 0, 80 },
    [BTN_PAUSE] = { "Pause", 84, 64 },
    [BTN_RESUME] = { "Resume", 152, 72 },
    { "spin", 290, 52 },
    { "primes", 346, 64 },
    { "ticker", 414, 64 },
};

#define BUTTON_COUNT ((int)(sizeof(buttons) / sizeof(buttons[0])))

static int list_h(int ch)
{
    return ch - HEADER_H - BOTTOM_H - 4;
}

static int list_rows(int ch)
{
    int rows = (list_h(ch) - 4) / ROW_H;
    return rows < 1 ? 1 : rows;
}

/*
 * draws the task manager: the column headings, then a row for each task (the
 * selected one highlighted), the scrollbar, the buttons along the bottom, and
 * the status line. the cpu column is worked out from how much time each task
 * got since the last look.
 */
static void draw(struct window *win, int cx, int cy, int cw, int ch)
{
    (void)win;
    struct task *list[MAX_TASKS];
    int count = task_snapshot(list, MAX_TASKS);

    int lw = cw - SCROLLBAR_W;
    int ly = cy + HEADER_H, lh = list_h(ch);

    /* the column headings, drawn as a row of raised buttons */
    for (int i = 0; i < COLUMN_COUNT; i++) {
        int x = cx + columns[i].x;
        int w = (i + 1 < COLUMN_COUNT ? columns[i + 1].x : lw) - columns[i].x;
        gfx_fill(x, cy, w, HEADER_H, C_FACE);
        gfx_bevel(x, cy, w, HEADER_H, true);
        gfx_text(x + 5, cy + 1, columns[i].label, C_BLACK);
    }

    gfx_fill(cx, ly, lw, lh, C_WHITE);
    gfx_bevel(cx, ly, lw, lh, false);

    int rows = list_rows(ch);
    sb.total = count;
    sb.visible = rows;
    ui_scroll_by(&sb, 0);

    gfx_clip(cx + 2, ly + 2, lw - 4, lh - 4);
    uint32_t hz = timer_hz();
    for (int i = 0; i < rows && sb.offset + i < count; i++) {
        struct task *t = list[sb.offset + i];
        int ry = ly + 2 + i * ROW_H;
        bool sel = t->pid == selected_pid;
        uint32_t text = sel ? C_WHITE : C_BLACK;
        char buf[16];

        if (sel)
            gfx_fill(cx + 2, ry, lw - 4, ROW_H, C_SELECT);

        ksnprintf(buf, sizeof(buf), "%d", t->pid);
        gfx_text(cx + columns[0].x + 5, ry + 1, buf, text);
        gfx_text(cx + columns[1].x + 5, ry + 1, t->name, text);
        gfx_text(cx + columns[2].x + 5, ry + 1, task_state_name(t), text);

        if (t->kind == TASK_THREAD) {
            ksnprintf(buf, sizeof(buf), "%3u%%", t->cpu_percent);
            gfx_text(cx + columns[3].x + 5, ry + 1, buf, text);
            uint32_t secs = t->ticks / hz;
            ksnprintf(buf, sizeof(buf), "%u:%02u", secs / 60, secs % 60);
            gfx_text(cx + columns[4].x + 5, ry + 1, buf, text);
        }
        gfx_text(cx + columns[5].x + 5, ry + 1, t->info, text);
    }
    gfx_clip(cx, cy, cw, ch);

    ui_scrollbar(cx + lw, ly, lh, &sb);

    int by = cy + ch - BOTTOM_H;
    for (int i = 0; i < BUTTON_COUNT; i++)
        ui_button(cx + buttons[i].x, by, buttons[i].w, BUTTON_H, buttons[i].label, false);
    gfx_text(cx + 238, by + 3, "Start:", C_BLACK);

    /* the idle task's share is whatever cpu nobody else wanted */
    uint32_t idle = 0;
    for (int i = 0; i < count; i++)
        if (list[i]->pid == 0)
            idle = list[i]->cpu_percent;
    char info[64];
    if (status[0])
        strlcpy(info, status, sizeof(info));
    else
        ksnprintf(info, sizeof(info), "%d tasks    CPU in use: %u%%", count, idle > 100 ? 0 : 100 - idle);
    int sy = cy + ch - STATUS_H;
    gfx_bevel(cx, sy, cw, STATUS_H, false);
    gfx_textn(cx + 6, sy + 2, info, (cw - 12) / FONT_W, C_BLACK);
}

static void act_on_selected(int (*action)(int pid), const char *done)
{
    if (selected_pid < 0) {
        strlcpy(status, "Select a task first.", sizeof(status));
        return;
    }
    int err = action(selected_pid);
    if (err < 0) {
        ksnprintf(status, sizeof(status), "pid %d: %s", selected_pid, task_error(err));
        gui_message(ICON_FAIL, "Task Manager", status);
    }
    else if (err == TASKMAN_ASKED)
        status[0] = '\0';
    else
        ksnprintf(status, sizeof(status), "%s pid %d", done, selected_pid);
}

/*
 * kernel tasks get the warning dialog first. the pid gets passed along as the
 * dialog's context, and only a yes ever reaches kill_confirmed.
 */
static void kill_confirmed(void *ctx)
{
    int pid = (int)(intptr_t)ctx;
    int err = task_kill_anyway(pid);
    if (err < 0)
        ksnprintf(status, sizeof(status), "pid %d: %s", pid, task_error(err));
    else
        ksnprintf(status, sizeof(status), "Ended kernel task pid %d", pid);
}

int taskman_kill(int pid)
{
    if (!task_is_kernel(pid))
        return task_kill(pid);
    gui_confirm(ICON_WARNING, "Kill kernel task", KERNEL_KILL_WARNING, kill_confirmed, (void *)(intptr_t)pid);
    return TASKMAN_ASKED;
}

/*
 * what each button does. end, pause and resume act on the selected task, and
 * the rest are named after a background program, which they start up (and
 * then select, so you can see it straight away).
 */
static void press_button(int b)
{
    if (b == BTN_END) {
        act_on_selected(taskman_kill, "Ended");
        return;
    }
    if (b == BTN_PAUSE) {
        act_on_selected(task_pause, "Paused");
        return;
    }
    if (b == BTN_RESUME) {
        act_on_selected(task_resume, "Resumed");
        return;
    }

    const struct program *prog = program_find(buttons[b].label);
    int pid = prog ? task_spawn(prog->name, prog->main) : TASK_ERR_NO_SUCH;
    if (pid < 0) {
        ksnprintf(status, sizeof(status), "%s: %s", buttons[b].label, task_error(pid));
    } else {
        selected_pid = pid;
        ksnprintf(status, sizeof(status), "Started %s as pid %d", buttons[b].label, pid);
    }
}

/*
 * a click: one of the buttons along the bottom, the scrollbar, or a row in
 * the table, which selects that task.
 */
static bool click(struct window *win, int x, int y, bool double_click)
{
    (void)double_click;
    int ch = win->ch, lw = win->cw - SCROLLBAR_W;
    int ly = HEADER_H, lh = list_h(ch);
    sb.grab = -1;

    int by = ch - BOTTOM_H;
    if (y >= by && y < by + BUTTON_H) {
        for (int i = 0; i < BUTTON_COUNT; i++)
            if (x >= buttons[i].x && x < buttons[i].x + buttons[i].w)
                press_button(i);
        return true;
    }

    if (y < ly || y >= ly + lh)
        return false;
    if (x >= lw)
        return ui_scrollbar_click(&sb, y - ly, lh);

    struct task *list[MAX_TASKS];
    int count = task_snapshot(list, MAX_TASKS);
    int index = sb.offset + (y - ly - 2) / ROW_H;
    status[0] = '\0';
    selected_pid = y >= ly + 2 && index < count ? list[index]->pid : -1;
    return true;
}

static bool drag(struct window *win, int x, int y)
{
    (void)x;
    return ui_scrollbar_drag(&sb, y - HEADER_H, list_h(win->ch));
}

static bool scroll(struct window *win, int rows)
{
    (void)win;
    ui_scroll_by(&sb, rows);
    return true;
}

static bool key(struct window *win, char c)
{
    (void)win;
    return ui_scroll_key(&sb, c);
}
