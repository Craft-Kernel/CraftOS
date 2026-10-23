#include <console.h>
#include <cpu.h>
#include <fs.h>
#include <string.h>
#include <sysfiles.h>
#include <task.h>
#include <version.h>

/*
 * the craftos folder layout. it borrows the shape of a linux system, so
 * things are roughly where you'd expect them, but everything in them is
 * craftos's own:
 *
 *   /bin     every terminal command, one read-only file each (the
 *            terminal adds these itself, since it owns the command list)
 *   /boot    about the kernel that's running
 *   /dev     devices: null swallows anything written to it; clock and
 *            mouse are live
 *   /etc     settings: hostname, motd, os-release. you can edit these
 *   /home    home folders. crafter is you
 *   /proc    live system info (tasks included), made fresh on every read
 *   /tmp     scratch space
 *   /usr     shared docs, the wallpaper and icons
 *   /var     the boot log
 *
 * everything outside /home/crafter, /etc, /tmp and /var is read-only.
 */

static uint32_t mem_kib;

extern char kernel_end[];

/*
 * the pictures that assets.s builds into the kernel. each one has a label at
 * its start and another at its end.
 */
extern const char asset_wallpaper[], asset_wallpaper_end[];
extern const char asset_craft_16[], asset_craft_16_end[];
extern const char asset_craft_32[], asset_craft_32_end[];
extern const char asset_craft_48[], asset_craft_48_end[];

#define STATUS_ICON(n, s) extern const char asset_##n##_##s[], asset_##n##_##s##_end[];
#define STATUS_SIZES(n) STATUS_ICON(n, 16) STATUS_ICON(n, 32) STATUS_ICON(n, 40) STATUS_ICON(n, 48)
STATUS_SIZES(info)
STATUS_SIZES(question)
STATUS_SIZES(warning)
STATUS_SIZES(fail)
#undef STATUS_ICON
#undef STATUS_SIZES

/* ---- live files ---- */

static int gen_version(char *buf, int size)
{
    return ksnprintf(buf, size, "%s %s (%s) i686, %s\n", OS_NAME, OS_VERSION, KERNEL_NAME, RELEASE);
}

static int gen_uptime(char *buf, int size)
{
    uint32_t t = timer_ticks(), hz = timer_hz();
    uint32_t s = t / hz;
    return ksnprintf(buf, size, "%u.%02u seconds (%u:%02u:%02u)\n",
                     s, t % hz * 100 / hz, s / 3600, s / 60 % 60, s % 60);
}

static int gen_meminfo(char *buf, int size)
{
    uint32_t image = ((uint32_t)kernel_end - 0x100000) / 1024;
    return ksnprintf(buf, size,
                     "total:        %u KiB\n"
                     "kernel image: %u KiB\n",
                     mem_kib + 1024, image);
}

/*
 * asking the cpu about itself. leaf 0 gives the maker's name, twelve letters
 * spread over ebx, edx and ecx. leaves 0x80000002 to 0x80000004 give the full
 * model name, if the cpu knows it.
 */
static void cpuid(uint32_t leaf, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf));
}

/*
 * the contents of /proc/cpuinfo: who made the cpu and what model it is, with
 * the padding trimmed off the front of the model name.
 */
static int gen_cpuinfo(char *buf, int size)
{
    uint32_t r[4];
    char vendor[13];
    cpuid(0, r);
    memcpy(vendor, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    vendor[12] = '\0';

    char brand[49] = "unknown";
    cpuid(0x80000000, r);
    if (r[0] >= 0x80000004) {
        for (uint32_t i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, r);
            memcpy(brand + i * 16, r, 16);
        }
        brand[48] = '\0';
    }

    char *b = brand;
    while (*b == ' ')
        b++;
    return ksnprintf(buf, size, "vendor: %s\nmodel:  %s\n", vendor, b);
}

static int gen_tasks(char *buf, int size)
{
    return task_describe(buf, size);
}

static int gen_clock(char *buf, int size)
{
    int h, m, s;
    rtc_time(&h, &m, &s);
    return ksnprintf(buf, size, "%02u:%02u:%02u\n", h, m, s);
}

static int gen_mouse(char *buf, int size)
{
    int x, y, wheel;
    uint8_t buttons;
    mouse_peek(&x, &y, &buttons, &wheel);
    return ksnprintf(buf, size, "x=%d y=%d buttons=%u\n", x, y, buttons);
}

static int gen_null(char *buf, int size)
{
    (void)size;
    buf[0] = '\0';
    return 0;
}

static int gen_boot_log(char *buf, int size)
{
    int len;
    const char *log = console_log(&len);
    if (len > size - 1)
        len = size - 1;
    memcpy(buf, log, len);
    buf[len] = '\0';
    return len;
}

static int gen_kernel_info(char *buf, int size)
{
    return ksnprintf(buf, size,
                     "%s %s\n"
                     "loaded at 0x00100000 by a multiboot bootloader\n"
                     "image ends at %p (%u KiB)\n",
                     KERNEL_NAME, OS_VERSION, kernel_end,
                     ((uint32_t)kernel_end - 0x100000) / 1024);
}

/*
 * builds the whole folder layout at boot: the folders, the settings and
 * welcome files, the live files in /proc and /dev, and all the pictures,
 * fonts and pages that are built into the kernel.
 */

void sysfiles_init(uint32_t mem_upper_kib)
{
    mem_kib = mem_upper_kib;

    fs_add_dir("/bin", true);
    fs_add_dir("/boot", true);
    fs_add_dir("/dev", true);
    fs_add_dir("/etc", false);
    fs_add_dir("/home", true);
    fs_add_dir("/home/crafter", false);
    fs_add_dir("/home/crafter/projects", false);
    fs_add_dir("/proc", true);
    fs_add_dir("/tmp", false);
    fs_add_dir("/usr", true);
    fs_add_dir("/usr/share", true);
    fs_add_dir("/usr/share/doc", true);
    fs_add_dir("/var", false);
    fs_add_dir("/var/log", false);

    fs_add_live("/boot/craft-kernel.info", gen_kernel_info);

    int null = fs_add_live("/dev/null", gen_null);
    fs_node(null)->readonly = false;
    fs_node(null)->discard_writes = true;
    fs_add_live("/dev/clock", gen_clock);
    fs_add_live("/dev/mouse", gen_mouse);

    fs_add_file("/etc/hostname", "craftbox\n", false);
    fs_add_file("/etc/motd",
                "Welcome to " OS_NAME " " OS_VERSION "!\n"
                "Type 'ui' to start the desktop, 'help' for commands, or\n"
                "'cat ~/readme.txt' to get started.\n",
                false);
    fs_add_file("/etc/os-release",
                "NAME=\"" OS_NAME "\"\n"
                "VERSION=\"" OS_VERSION "\"\n"
                "ID=craftos\n"
                "PRETTY_NAME=\"" OS_NAME " " OS_VERSION " (" KERNEL_NAME ")\"\n",
                false);

    fs_add_file(FS_HOME "/readme.txt",
                "Welcome to CraftOS!\n"
                "\n"
                "This is your home folder, ~ for short. Everything on the\n"
                "disk lives in RAM, so it starts fresh every time you boot.\n"
                "\n"
                "Have a look around the rest of the system too: / has bin,\n"
                "etc, proc and the other folders you'd expect. Most of it is\n"
                "read-only, but /etc, /tmp and your home folder are yours.\n",
                false);
    fs_add_file(FS_HOME "/notes.txt",
                "Ideas for CraftOS:\n"
                "- a paint program\n"
                "- saving files to a real disk\n"
                "- a few small games\n",
                false);

    fs_add_live("/proc/version", gen_version);
    fs_add_live("/proc/uptime", gen_uptime);
    fs_add_live("/proc/meminfo", gen_meminfo);
    fs_add_live("/proc/cpuinfo", gen_cpuinfo);
    fs_add_live("/proc/tasks", gen_tasks);

    fs_add_file("/usr/share/doc/keys.txt",
                "Keys and mouse\n"
                "\n"
                "F11            full screen on/off for the focused window\n"
                "Ctrl+Alt+Del   open the Task Manager\n"
                "PgUp / PgDn    scroll a page\n"
                "Up / Down      scroll a line\n"
                "Mouse wheel    scroll the window under the pointer\n"
                "\n"
                "Drag a window's right or bottom edge (or the corner) to\n"
                "resize it. Double-click a title bar to maximize it.\n",
                true);
    fs_add_file("/usr/share/doc/layout.txt",
                "Where things are\n"
                "\n"
                "/bin     the terminal's commands\n"
                "/boot    the running kernel\n"
                "/dev     devices (null, clock, mouse)\n"
                "/etc     settings you can edit\n"
                "/home    home folders\n"
                "/proc    live system info\n"
                "/tmp     scratch space\n"
                "/usr     shared docs, the wallpaper and icons\n"
                "/var     logs\n",
                true);

    fs_add_live("/var/log/boot.log", gen_boot_log);

    fs_add_dir("/usr/share/wallpapers", true);
    fs_add_dir("/usr/share/icons", true);
    fs_add_blob("/usr/share/wallpapers/craft.cimg", asset_wallpaper, asset_wallpaper_end - asset_wallpaper);
    fs_add_blob("/usr/share/icons/craft-16.cimg", asset_craft_16, asset_craft_16_end - asset_craft_16);
    fs_add_blob("/usr/share/icons/craft-32.cimg", asset_craft_32, asset_craft_32_end - asset_craft_32);
    fs_add_blob("/usr/share/icons/craft-48.cimg", asset_craft_48, asset_craft_48_end - asset_craft_48);

    /*
     * the status icons, from the craftos status icons design:
     * /usr/share/icons/status/info-32.cimg and so on
     */
    fs_add_dir("/usr/share/icons/status", true);
#define STATUS_ICON(n, s)     fs_add_blob("/usr/share/icons/status/" #n "-" #s ".cimg", asset_##n##_##s, asset_##n##_##s##_end - asset_##n##_##s);
#define STATUS_SIZES(n) STATUS_ICON(n, 16) STATUS_ICON(n, 32) STATUS_ICON(n, 40) STATUS_ICON(n, 48)
    STATUS_SIZES(info)
    STATUS_SIZES(question)
    STATUS_SIZES(warning)
    STATUS_SIZES(fail)
#undef STATUS_ICON
#undef STATUS_SIZES
}
