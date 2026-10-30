#include <apps.h>
#include <console.h>
#include <cpu.h>
#include <fs.h>
#include <gui.h>
#include <heap.h>
#include <http.h>
#include <io.h>
#include <net.h>
#include <programs.h>
#include <shell.h>
#include <stdarg.h>
#include <string.h>
#include <task.h>
#include <trash.h>
#include <version.h>

/*
 * the shell finds its commands through /bin. each command is a read-only file
 * in there holding its usage line, and the shell will only run a command
 * whose file actually exists. most commands are built right into the shell,
 * and the rest are programs, which start up as background tasks.
 *
 * a few commands need the desktop (edit, files, taskman) and a few only make
 * sense outside it (sleep would freeze it), and they say so instead of doing
 * anything odd.
 */

#define USER "crafter"

/*
 * every command gets the usual argc and argv, plus rest: the raw text after
 * the command's name, for echo, which needs the spacing kept just as it was
 * typed.
 */
typedef void (*cmd_fn)(struct shell *sh, int argc, char **argv, char *rest);

static void shprintf(struct shell *sh, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void shprintf(struct shell *sh, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sh->write(buf, len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1);
}

static void fail(struct shell *sh, const char *cmd, const char *what, int err)
{
    shprintf(sh, "%s: %s: %s\n", cmd, what, fs_error(err));
}

static bool needs_desktop(struct shell *sh, const char *cmd)
{
    if (sh->in_desktop)
        return false;
    shprintf(sh, "%s: this needs the desktop. Type 'ui' to start it.\n", cmd);
    return true;
}

static char *trim(char *s)
{
    while (*s == ' ')
        s++;
    char *end = s + strlen(s);
    while (end > s && end[-1] == ' ')
        *--end = '\0';
    return s;
}

/* ---- the current folder and the prompt ---- */

static void set_cwd(struct shell *sh, int n)
{
    sh->cwd = n;
    sh->cwd_id = fs_node(n)->id;
}

/*
 * if something deleted the folder we're in, fall back to the home folder, or
 * the root.
 */
static void check_cwd(struct shell *sh)
{
    if (fs_alive(sh->cwd, sh->cwd_id))
        return;
    int home = fs_resolve(FS_ROOT, FS_HOME);
    set_cwd(sh, home >= 0 ? home : FS_ROOT);
}

/* the first line of /etc/hostname, so editing that file renames the machine. */
static void hostname(char *buf, int size)
{
    char text[64];
    int n = fs_resolve(FS_ROOT, "/etc/hostname");
    int len = n >= 0 ? fs_read(n, text, sizeof(text)) : 0;
    int i = 0;
    while (i < len && i < size - 1 && text[i] != '\n')
        i++;
    if (i == 0) {
        strlcpy(buf, "craftbox", size);
        return;
    }
    memcpy(buf, text, i);
    buf[i] = '\0';
}

void shell_prompt(struct shell *sh)
{
    char path[128], host[32];
    check_cwd(sh);
    fs_short_path(sh->cwd, path, sizeof(path));
    hostname(host, sizeof(host));
    shprintf(sh, "%s@%s:%s$ ", USER, host, path);
}

/* ---- files ---- */

static void cmd_ls(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    const char *path = argc > 1 ? argv[1] : ".";
    int dir = fs_resolve(sh->cwd, path);
    if (dir < 0) {
        fail(sh, "ls", path, dir);
        return;
    }
    if (!fs_node(dir)->is_dir) {
        shprintf(sh, "%s\n", fs_node(dir)->name);
        return;
    }

    int children[FS_MAX_NODES];
    int count = fs_list(dir, children, FS_MAX_NODES);
    for (int i = 0; i < count; i++) {
        struct fs_node *n = fs_node(children[i]);
        const char *flags = n->generate ? "live" : n->readonly ? "read-only" : "";
        if (n->is_dir) {
            char name[FS_NAME_MAX + 1];
            ksnprintf(name, sizeof(name), "%s/", n->name);
            shprintf(sh, "  %-24s %8s  %s\n", name, "", flags);
        } else {
            shprintf(sh, "  %-24s %6u B  %s\n", n->name, fs_size(children[i]), flags);
        }
    }
    if (!count)
        shprintf(sh, "  (empty)\n");
}

static void cmd_cd(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    const char *path = argc > 1 ? argv[1] : "~";
    int n = fs_resolve(sh->cwd, path);
    if (n < 0)
        fail(sh, "cd", path, n);
    else if (!fs_node(n)->is_dir)
        fail(sh, "cd", path, FS_ERR_NOT_DIR);
    else
        set_cwd(sh, n);
}

static void cmd_pwd(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    char path[128];
    fs_path(sh->cwd, path, sizeof(path));
    shprintf(sh, "%s\n", path);
}

static void cmd_cat(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    static char buf[FS_FILE_MAX];
    if (argc < 2) {
        shprintf(sh, "usage: cat FILE\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        int n = fs_resolve(sh->cwd, argv[i]);
        int len = n < 0 ? n : fs_read(n, buf, sizeof(buf));
        if (len < 0) {
            fail(sh, "cat", argv[i], len);
            continue;
        }
        sh->write(buf, len);
        if (len && buf[len - 1] != '\n')
            sh->write("\n", 1);
    }
}

/*
 * echo text, echo text > file (to overwrite it) and echo text >> file (to add
 * to the end).
 */
static void cmd_echo(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv;
    char *redirect = strchr(rest, '>');
    if (!redirect) {
        shprintf(sh, "%s\n", rest);
        return;
    }

    bool append = redirect[1] == '>';
    *redirect = '\0';
    char *text = trim(rest);
    char *target = trim(redirect + (append ? 2 : 1));

    int n = fs_resolve(sh->cwd, target);
    if (n < 0)
        n = fs_create(sh->cwd, target, false);
    if (n < 0) {
        fail(sh, "echo", target, n);
        return;
    }
    int err = fs_write(n, text, strlen(text), append);
    if (err == 0)
        err = fs_write(n, "\n", 1, true);
    if (err < 0)
        fail(sh, "echo", target, err);
}

/*
 * mkdir and touch only differ in what they make, and touch doesn't mind if
 * the file's already there.
 */
static void make(struct shell *sh, int argc, char **argv, bool is_dir)
{
    if (argc < 2) {
        shprintf(sh, "usage: %s NAME\n", argv[0]);
        return;
    }
    for (int i = 1; i < argc; i++) {
        if (!is_dir && fs_resolve(sh->cwd, argv[i]) >= 0)
            continue;
        int n = fs_create(sh->cwd, argv[i], is_dir);
        if (n < 0)
            fail(sh, argv[0], argv[i], n);
    }
}

static void cmd_mkdir(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    make(sh, argc, argv, true);
}

static void cmd_touch(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    make(sh, argc, argv, false);
}

static void cmd_rm(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    if (argc < 2) {
        shprintf(sh, "usage: rm PATH\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        int n = fs_resolve(sh->cwd, argv[i]);
        int err = n < 0 ? n : fs_remove(n);
        if (err < 0)
            fail(sh, "rm", argv[i], err);
    }
}

static void cmd_mv(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    if (argc != 3) {
        shprintf(sh, "usage: mv FROM TO\n");
        return;
    }
    int n = fs_resolve(sh->cwd, argv[1]);
    int err = n < 0 ? n : fs_move(n, sh->cwd, argv[2]);
    if (err < 0)
        fail(sh, "mv", argv[1], err);
}

/* ---- desktop ---- */

static void cmd_edit(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    if (needs_desktop(sh, "edit"))
        return;
    if (argc < 2) {
        gui_open(&notepad_window);
        return;
    }
    int n = fs_resolve(sh->cwd, argv[1]);
    if (n < 0)
        n = fs_create(sh->cwd, argv[1], false);
    if (n < 0)
        fail(sh, "edit", argv[1], n);
    else if (fs_node(n)->is_dir)
        fail(sh, "edit", argv[1], FS_ERR_IS_DIR);
    else
        notepad_open_file(n);
}

static void cmd_files(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    if (needs_desktop(sh, "files"))
        return;
    const char *path = argc > 1 ? argv[1] : ".";
    int n = fs_resolve(sh->cwd, path);
    if (n < 0)
        fail(sh, "files", path, n);
    else if (!fs_node(n)->is_dir)
        fail(sh, "files", path, FS_ERR_NOT_DIR);
    else
        files_open_at(n);
}

static void cmd_view(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    if (needs_desktop(sh, "view"))
        return;
    if (argc < 2) {
        shprintf(sh, "usage: view IMAGE\n");
        return;
    }
    int n = fs_resolve(sh->cwd, argv[1]);
    if (n < 0)
        fail(sh, "view", argv[1], n);
    else if (fs_node(n)->is_dir)
        fail(sh, "view", argv[1], FS_ERR_IS_DIR);
    else
        imgview_open(n);
}

static void cmd_mines(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    if (!needs_desktop(sh, "mines"))
        gui_open(&mines_window);
}

static void cmd_solitaire(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    if (!needs_desktop(sh, "solitaire"))
        gui_open(&solitaire_window);
}

static void cmd_taskman(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    if (!needs_desktop(sh, "taskman"))
        gui_open(&taskman_window);
}

/*
 * ui starts the desktop from the text screen, and only comes back once the
 * desktop's been closed again. from inside the desktop, `ui exit` closes it.
 */
static void cmd_ui(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    bool exiting = argc > 1 && strcmp(argv[1], "exit") == 0;
    if (sh->in_desktop) {
        if (exiting)
            gui_request_exit();
        else
            shprintf(sh, "ui: the desktop is already running. 'ui exit' goes back to text mode.\n");
        return;
    }
    if (exiting) {
        shprintf(sh, "ui: the desktop isn't running.\n");
        return;
    }
    if (!gui_start())
        shprintf(sh, "ui: no BGA graphics card found, so it's text mode only.\n");
}

static void cmd_exit(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    if (sh->in_desktop)
        gui_close(&terminal_window);
    else
        shprintf(sh, "exit: this is the main console. 'shutdown' turns the machine off.\n");
}

/* ---- tasks ---- */

static void cmd_ps(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    static char buf[2048];
    int len = task_describe(buf, sizeof(buf));
    sh->write(buf, len);
}

/* kill, pause and resume all take pids or names, and they share this loop. */
static void each_task(struct shell *sh, int argc, char **argv, int (*action)(int pid), const char *done)
{
    if (argc < 2) {
        shprintf(sh, "usage: %s PID|NAME...\n", argv[0]);
        return;
    }
    for (int i = 1; i < argc; i++) {
        int pid = task_lookup(argv[i]);
        int err = pid < 0 ? pid : action(pid);
        if (err < 0)
            shprintf(sh, "%s: %s: %s\n", argv[0], argv[i], task_error(err));
        else
            shprintf(sh, "%s pid %d\n", done, pid);
    }
}

/*
 * the text screen has no windows, so the kernel task warning becomes a y/n
 * question right there. in the desktop it's the warning dialog instead.
 */
static bool ask_yes_no(struct shell *sh)
{
    shprintf(sh, "(y/n) ");
    char c;
    do
        c = keyboard_getchar();
    while (c != 'y' && c != 'Y' && c != 'n' && c != 'N' && c != '\n');
    shprintf(sh, "%c\n", c == '\n' ? 'n' : c);
    return c == 'y' || c == 'Y';
}

/*
 * kill: ends each task named, by pid or by name. ordinary tasks just end. for
 * a kernel task, the desktop pops up its warning dialog, and the text screen
 * prints the warning and asks y/n first, since killing pid 1 brings the whole
 * kernel down.
 */
static void cmd_kill(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    if (argc < 2) {
        shprintf(sh, "usage: kill PID|NAME...\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        int pid = task_lookup(argv[i]);
        if (pid < 0) {
            shprintf(sh, "kill: %s: %s\n", argv[i], task_error(pid));
            continue;
        }

        int err;
        if (!task_is_kernel(pid)) {
            err = task_kill(pid);
        } else if (sh->in_desktop) {
            taskman_kill(pid);
            shprintf(sh, "kill: pid %d is a kernel task. Answer the warning to go on.\n", pid);
            continue;
        } else {
            shprintf(sh, "%s ", KERNEL_KILL_WARNING);
            if (!ask_yes_no(sh)) {
                shprintf(sh, "left pid %d alone\n", pid);
                continue;
            }
            err = task_kill_anyway(pid);
        }

        if (err < 0)
            shprintf(sh, "kill: %s: %s\n", argv[i], task_error(err));
        else
            shprintf(sh, "ended pid %d\n", pid);
    }
}

static void cmd_pause(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    each_task(sh, argc, argv, task_pause, "paused");
}

static void cmd_resume(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    each_task(sh, argc, argv, task_resume, "resumed");
}

/* the shell's thread sleeps, and background tasks carry on meanwhile. */
static void cmd_sleep(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    if (sh->in_desktop) {
        shprintf(sh, "sleep: that would freeze the desktop. It works in text mode.\n");
        return;
    }
    int secs = 0;
    for (const char *p = argc > 1 ? argv[1] : ""; *p >= '0' && *p <= '9'; p++)
        secs = secs * 10 + (*p - '0');
    if (secs <= 0) {
        shprintf(sh, "usage: sleep SECONDS\n");
        return;
    }
    task_sleep(secs * timer_hz());
}

/* ---- system ---- */

static void cmd_clear(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    sh->clear();
}

static void cmd_uname(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    char host[32];
    hostname(host, sizeof(host));
    shprintf(sh, "%s %s %s (%s) i686\n", OS_NAME, host, OS_VERSION, KERNEL_NAME);
}

static void cmd_whoami(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    shprintf(sh, "%s\n", USER);
}

/*
 * given a name, it writes it to /etc/hostname, and the prompt picks it up
 * straight away.
 */
static void cmd_hostname(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    char host[32];
    if (argc < 2) {
        hostname(host, sizeof(host));
        shprintf(sh, "%s\n", host);
        return;
    }
    int n = fs_resolve(FS_ROOT, "/etc/hostname");
    if (n < 0)
        n = fs_create(FS_ROOT, "/etc/hostname", false);
    ksnprintf(host, sizeof(host), "%s\n", argv[1]);
    int err = n < 0 ? n : fs_write(n, host, strlen(host), false);
    if (err < 0)
        fail(sh, "hostname", "/etc/hostname", err);
}

static void cmd_date(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    int y, mo, d, h, mi, s;
    rtc_date(&y, &mo, &d);
    rtc_time(&h, &mi, &s);
    shprintf(sh, "%04u-%02u-%02u %02u:%02u:%02u\n", y, mo, d, h, mi, s);
}

static void cmd_uptime(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    uint32_t up = timer_ticks() / timer_hz();
    shprintf(sh, "up %u:%02u:%02u\n", up / 3600, up / 60 % 60, up % 60);
}

static void cmd_free(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    int used = 0;
    for (int i = 0; i < FS_MAX_NODES; i++)
        used += fs_node(i)->used;
    shprintf(sh, "memory:   %u MiB\n", system_mem_kib() / 1024);
    shprintf(sh, "ram disk: %d of %d entries used\n", used, FS_MAX_NODES);
}

/*
 * gcc turns a plain `1 / x` into a comparison that never faults, so the
 * divide instruction gets written out by hand.
 */
static void cmd_crash(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)sh, (void)argc, (void)argv, (void)rest;
    __asm__ volatile("xor %%ecx, %%ecx; div %%ecx" : : : "eax", "ecx", "edx");
}

static void cmd_reboot(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)sh, (void)argc, (void)argv, (void)rest;
    machine_reboot();
}

static void cmd_shutdown(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    if (sh->in_desktop)
        gui_shut_down();
    shprintf(sh, "Shutting down...\n");
    machine_poweroff();
    shprintf(sh, "CraftOS has shut down. You can turn off your computer now.\n");
    halt_forever();
}

/* ---- network ---- */

/*
 * network commands have to wait on the network, which would freeze the
 * desktop if the terminal ran them on its own thread. so in the desktop they
 * run as a background job, in a task of their own. the shell gets marked busy
 * (so the terminal holds back its prompt and ignores typing), and when the
 * job's done, it prints the prompt itself. on the text screen they just run
 * normally.
 */

static struct {
    struct shell *sh;
    cmd_fn fn;
    char line[SHELL_LINE_MAX + 1];
    char *argv[8];
    int argc;
} job;

static void job_main(void)
{
    job.fn(job.sh, job.argc, job.argv, "");
    job.sh->busy = false;
    shell_prompt(job.sh);
    gui_poke();
}

/*
 * runs a slow command. on the text screen it just runs. in the desktop, it
 * copies the arguments, marks the shell busy, and starts the command in a
 * task of its own, which prints the prompt again when it's done.
 */
static void run_slow(struct shell *sh, const char *name, cmd_fn fn, int argc, char **argv)
{
    if (!sh->in_desktop) {
        fn(sh, argc, argv, "");
        return;
    }

    /*
     * the arguments point into the line being typed, so the job keeps a copy
     * of its own
     */
    int len = 0;
    job.argc = 0;
    for (int i = 0; i < argc && i < 8; i++) {
        int n = strlen(argv[i]);
        if (len + n + 1 > SHELL_LINE_MAX)
            break;
        memcpy(job.line + len, argv[i], n + 1);
        job.argv[job.argc++] = job.line + len;
        len += n + 1;
    }
    job.sh = sh;
    job.fn = fn;
    sh->busy = true;
    int pid = task_spawn(name, job_main);
    if (pid < 0) {
        sh->busy = false;
        shprintf(sh, "%s: %s\n", name, task_error(pid));
    }
}

static bool net_ready(struct shell *sh, const char *cmd)
{
    if (!net.card) {
        shprintf(sh, "%s: %s\n", cmd, net_error(NET_ERR_NO_CARD));
        return false;
    }
    return true;
}

static void cmd_ifconfig(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    static char buf[512];
    net_describe(buf, sizeof(buf));
    shprintf(sh, "%s", buf);
}

static void do_dhcp(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    shprintf(sh, "Asking the network for an address...\n");
    if (!net_dhcp()) {
        shprintf(sh, "dhcp: no answer from a dhcp server\n");
        return;
    }
    char ip[16], gw[16];
    net_format_ip(net.ip, ip);
    net_format_ip(net.gateway, gw);
    shprintf(sh, "Got %s, gateway %s\n", ip, gw);
}

static void cmd_dhcp(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    if (net_ready(sh, "dhcp"))
        run_slow(sh, "dhcp", do_dhcp, argc, argv);
}

/*
 * ping: looks the name up, then sends count pings (4 unless you say),
 * printing how long each reply took, or that it timed out, and a summary at
 * the end.
 */
static void do_ping(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    int count = 4;
    if (argc > 2) {
        count = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
            count = count * 10 + (*p - '0');
        if (count <= 0 || count > 100)
            count = 4;
    }
    uint32_t ip;
    int err = net_resolve(argv[1], &ip);
    if (err < 0) {
        shprintf(sh, "ping: %s: %s\n", argv[1], net_error(err));
        return;
    }
    char addr[16];
    net_format_ip(ip, addr);
    shprintf(sh, "PING %s (%s)\n", argv[1], addr);

    int got = 0;
    uint32_t ms_per_tick = 1000 / timer_hz();
    for (int i = 0; i < count; i++) {
        uint32_t start = timer_ticks();
        int rtt = net_ping(ip, i + 1, 2 * timer_hz());
        if (rtt >= 0) {
            got++;
            shprintf(sh, "reply from %s: seq=%d time=%u ms\n", addr, i + 1, rtt * ms_per_tick);
        } else {
            shprintf(sh, "seq=%d: %s\n", i + 1, net_error(rtt));
        }
        gui_poke();
        while (i + 1 < count && timer_ticks() - start < timer_hz())
            task_sleep(1);
    }
    shprintf(sh, "%d sent, %d received\n", count, got);
}

static void cmd_ping(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    if (argc < 2) {
        shprintf(sh, "usage: ping HOST [COUNT]\n");
        return;
    }
    if (net_ready(sh, "ping"))
        run_slow(sh, "ping", do_ping, argc, argv);
}

static void do_host(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)rest;
    uint32_t ip;
    int err = net_resolve(argv[1], &ip);
    if (err < 0) {
        shprintf(sh, "host: %s: %s\n", argv[1], net_error(err));
        return;
    }
    char addr[16];
    net_format_ip(ip, addr);
    shprintf(sh, "%s has address %s\n", argv[1], addr);
}

static void cmd_host(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    if (argc < 2) {
        shprintf(sh, "usage: host NAME\n");
        return;
    }
    if (net_ready(sh, "host"))
        run_slow(sh, "host", do_host, argc, argv);
}

static struct shell *wget_sh;

static void wget_progress(const char *msg)
{
    (void)msg;
    gui_poke();
}

/*
 * the file gets named after the last part of the url, or index.html for a
 * folder.
 */
static void do_wget(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    wget_sh = sh;
    char url[URL_MAX];
    if (strstr(argv[1], "://"))
        strlcpy(url, argv[1], sizeof(url));
    else
        ksnprintf(url, sizeof(url), "http://%s", argv[1]);     /* plain http, since there's no https here */

    shprintf(sh, "Fetching %s\n", url);
    static struct http_result res;
    int err = http_get(url, &res, wget_progress);
    if (err < 0) {
        shprintf(sh, "wget: %s\n", res.error);
        return;
    }

    char name[FS_NAME_MAX];
    if (argc > 2) {
        strlcpy(name, argv[2], sizeof(name));
    } else {
        struct url u;
        url_parse(res.final_url, &u);
        char *q = strchr(u.path, '?');
        if (q)
            *q = '\0';
        const char *last = strrchr(u.path, '/');
        last = last ? last + 1 : u.path;
        strlcpy(name, *last ? last : "index.html", sizeof(name));
    }

    int n = fs_resolve(sh->cwd, name);
    if (n < 0)
        n = fs_create(sh->cwd, name, false);
    if (n < 0) {
        fail(sh, "wget", name, n);
    } else {
        int werr = fs_write(n, res.body, res.len, false);
        if (werr < 0)
            fail(sh, "wget", name, werr);
        else
            shprintf(sh, "HTTP %d, saved %d bytes to %s%s\n", res.status, res.len, name,
                     res.len >= FS_BIG_MAX ? " (cut off at the 1 MiB file limit)" : "");
    }
    http_free(&res);
}

static void cmd_wget(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    if (argc < 2) {
        shprintf(sh, "usage: wget URL [FILE]\n");
        return;
    }
    if (net_ready(sh, "wget"))
        run_slow(sh, "wget", do_wget, argc, argv);
}

static void cmd_trash(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)rest;
    if (argc < 2) {
        shprintf(sh, "usage: trash PATH...  (empty it with 'trash --empty')\n");
        return;
    }
    if (!strcmp(argv[1], "--empty")) {
        int n = trash_empty();
        shprintf(sh, "Emptied the Recycle Bin (%d item%s)\n", n, n == 1 ? "" : "s");
        return;
    }
    for (int i = 1; i < argc; i++) {
        int n = fs_resolve(sh->cwd, argv[i]);
        int err = n < 0 ? n : trash_put(n);
        if (err < 0)
            fail(sh, "trash", argv[i], err);
    }
}

/* ---- the command table ---- */

static void cmd_help(struct shell *sh, int argc, char **argv, char *rest);

static const struct {
    const char *name;
    const char *usage;
    cmd_fn fn;
} commands[] = {
    { "cat", "cat FILE...  print files", cmd_cat },
    { "cd", "cd [PATH]  change folder (default ~)", cmd_cd },
    { "clear", "clear  clear the screen", cmd_clear },
    { "crash", "crash  divide by zero, to test the exception handler", cmd_crash },
    { "date", "date  the current date and time", cmd_date },
    { "dhcp", "dhcp  ask the network for an address again", cmd_dhcp },
    { "echo", "echo TEXT [> FILE | >> FILE]  print or write text", cmd_echo },
    { "edit", "edit FILE  open a file in Notepad (desktop)", cmd_edit },
    { "exit", "exit  close the terminal window (desktop)", cmd_exit },
    { "files", "files [PATH]  open the file manager (desktop)", cmd_files },
    { "free", "free  memory and ram disk use", cmd_free },
    { "help", "help  list the commands in /bin", cmd_help },
    { "host", "host NAME  look up a name's address (dns)", cmd_host },
    { "hostname", "hostname [NAME]  show or set the machine name", cmd_hostname },
    { "ifconfig", "ifconfig  network card and address", cmd_ifconfig },
    { "kill", "kill PID|NAME...  end tasks", cmd_kill },
    { "ls", "ls [PATH]  list a folder", cmd_ls },
    { "mines", "mines  play minesweeper (desktop)", cmd_mines },
    { "mkdir", "mkdir NAME...  make folders", cmd_mkdir },
    { "mv", "mv FROM TO  move or rename", cmd_mv },
    { "pause", "pause PID|NAME...  stop tasks from running for now", cmd_pause },
    { "ping", "ping HOST [COUNT]  check that a machine answers", cmd_ping },
    { "ps", "ps  list tasks", cmd_ps },
    { "pwd", "pwd  show the current folder", cmd_pwd },
    { "reboot", "reboot  restart the machine", cmd_reboot },
    { "resume", "resume PID|NAME...  let paused tasks run again", cmd_resume },
    { "rm", "rm PATH...  delete files or empty folders", cmd_rm },
    { "shutdown", "shutdown  turn the machine off", cmd_shutdown },
    { "sleep", "sleep SECONDS  wait (text mode)", cmd_sleep },
    { "solitaire", "solitaire  play solitaire (desktop)", cmd_solitaire },
    { "taskman", "taskman  open the task manager (desktop)", cmd_taskman },
    { "touch", "touch NAME...  make empty files", cmd_touch },
    { "trash", "trash PATH...  move to the Recycle Bin (--empty empties it)", cmd_trash },
    { "ui", "ui [exit]  start the desktop, or leave it", cmd_ui },
    { "uname", "uname  system name and version", cmd_uname },
    { "uptime", "uptime  time since boot", cmd_uptime },
    { "view", "view IMAGE  open an image in the image viewer (desktop)", cmd_view },
    { "wget", "wget URL [FILE]  save an http:// page to a file", cmd_wget },
    { "whoami", "whoami  your user name", cmd_whoami },
};

#define CMD_COUNT ((int)(sizeof(commands) / sizeof(commands[0])))

/* help reads /bin rather than the table, so it lists what's really there. */
static void cmd_help(struct shell *sh, int argc, char **argv, char *rest)
{
    (void)argc, (void)argv, (void)rest;
    static char usage[256];
    int bin = fs_resolve(FS_ROOT, "/bin");
    int children[FS_MAX_NODES];
    int count = bin < 0 ? 0 : fs_list(bin, children, FS_MAX_NODES);

    for (int i = 0; i < count; i++) {
        int len = fs_read(children[i], usage, sizeof(usage));
        if (len > 0 && usage[len - 1] == '\n')
            usage[len - 1] = '\0';
        shprintf(sh, "  %s\n", usage);
    }
}

void shell_install(void)
{
    char path[32], usage[96];
    for (int i = 0; i < CMD_COUNT; i++) {
        ksnprintf(path, sizeof(path), "/bin/%s", commands[i].name);
        ksnprintf(usage, sizeof(usage), "%s\n", commands[i].usage);
        fs_add_file(path, usage, true);
    }
    for (int i = 0; i < program_count; i++) {
        ksnprintf(path, sizeof(path), "/bin/%s", programs[i].name);
        ksnprintf(usage, sizeof(usage), "%s\n", programs[i].usage);
        fs_add_file(path, usage, true);
    }
}

/*
 * the name of the /bin file a command word points at, or null if there isn't
 * one. a path like /bin/ls works too, as long as it really points into /bin.
 */
static const char *find_in_bin(struct shell *sh, const char *word)
{
    char path[64];
    if (strchr(word, '/'))
        strlcpy(path, word, sizeof(path));
    else
        ksnprintf(path, sizeof(path), "/bin/%s", word);

    int n = fs_resolve(sh->cwd, path);
    int bin = fs_resolve(FS_ROOT, "/bin");
    if (n < 0 || bin < 0 || fs_node(n)->is_dir || fs_node(n)->parent != bin)
        return 0;
    return fs_node(n)->name;
}

/*
 * runs one line: splits it into words in place (keeping a copy for the raw
 * rest of the line), finds the command in /bin, and runs it.
 */
void shell_run(struct shell *sh, char *line)
{
    char raw[SHELL_LINE_MAX + 1];
    strlcpy(raw, line, sizeof(raw));

    char *argv[8];
    int argc = 0;
    for (char *p = line; *p && argc < 8;) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        argv[argc++] = p;
        while (*p && *p != ' ')
            p++;
        if (*p)
            *p++ = '\0';
    }
    if (!argc)
        return;

    char *rest = raw;
    while (*rest == ' ')
        rest++;
    while (*rest && *rest != ' ')
        rest++;
    while (*rest == ' ')
        rest++;

    check_cwd(sh);
    const char *name = find_in_bin(sh, argv[0]);
    if (!name) {
        shprintf(sh, "%s: command not found (try 'help')\n", argv[0]);
        return;
    }

    for (int i = 0; i < CMD_COUNT; i++) {
        if (strcmp(name, commands[i].name) == 0) {
            commands[i].fn(sh, argc, argv, rest);
            return;
        }
    }

    const struct program *prog = program_find(name);
    if (prog) {
        int pid = task_spawn(prog->name, prog->main);
        if (pid < 0)
            shprintf(sh, "%s: %s\n", name, task_error(pid));
        else
            shprintf(sh, "[%d] %s started in the background. 'kill %d' stops it.\n", pid, name, pid);
    }
}

void shell_start(struct shell *sh)
{
    int home = fs_resolve(FS_ROOT, FS_HOME);
    set_cwd(sh, home >= 0 ? home : FS_ROOT);

    static char motd[512];
    int n = fs_resolve(FS_ROOT, "/etc/motd");
    int len = n >= 0 ? fs_read(n, motd, sizeof(motd)) : 0;
    if (len > 0) {
        sh->write(motd, len);
        sh->write("\n", 1);
    }
}
