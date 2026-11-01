#include <console.h>
#include <cpu.h>
#include <fs.h>
#include <gfx.h>
#include <heap.h>
#include <net.h>
#include <sb16.h>
#include <shell.h>
#include <speaker.h>
#include <string.h>
#include <sysfiles.h>
#include <task.h>
#include <version.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/*
 * just the start of the multiboot info the bootloader hands us. mem_lower and
 * mem_upper are how many kib of ram there are below and above 1 mib; they're
 * only filled in if bit 0 of flags is set.
 */
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
};

static uint32_t mem_upper_kb;

uint32_t system_mem_kib(void)
{
    return mem_upper_kb;
}

/*
 * the text console's side of the shell. output goes straight to the screen,
 * and lines are typed at the keyboard with the simplest editing there is
 * (backspace, and that's it).
 */

static void console_write_n(const char *s, int len)
{
    for (int i = 0; i < len; i++)
        console_putc(s[i]);
}

static struct shell console_shell = { .write = console_write_n, .clear = console_clear };

/*
 * reads a line from the keyboard into buf, echoing it as it's typed, until
 * enter is pressed. only printable characters go in, backspace rubs out the
 * last one, and up and down swap the line for an earlier command from the
 * history (rubbing out what's there and typing the new one in its place).
 */
static void read_line(char *buf, int max)
{
    struct shell_recall recall = { 0 };
    int len = 0;
    for (;;) {
        char c = keyboard_getchar();
        if (c == KEY_UP || c == KEY_DOWN) {
            int old = len;
            if (shell_history_step(&recall, c == KEY_UP ? -1 : 1, buf, &len)) {
                while (old--)
                    console_putc('\b');
                for (int i = 0; i < len; i++)
                    console_putc(buf[i]);
            }
            continue;
        }
        if (c == '\n') {
            console_putc('\n');
            buf[len] = '\0';
            return;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                console_putc('\b');
            }
        } else if (c >= ' ' && c <= '~' && len < max - 1) {
            buf[len++] = c;
            console_putc(c);
        }
    }
}

static void ok(const char *what)
{
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("[ OK ] ");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("%s\n", what);
}

/*
 * this is where everything starts, called from the little assembly entry
 * point. it brings things up one at a time, saying ok as each one works: the
 * cpu tables, the drivers, the ram disk and the scheduler. then it switches
 * interrupts on, starts the network, and hands the console over to the shell.
 * craftos always starts here in text mode; typing `ui` at the shell starts
 * the desktop.
 */
void kernel_main(uint32_t magic, struct multiboot_info *mbi)
{
    console_init();

    console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("%s %s (%s), %s\n\n", OS_NAME, OS_VERSION, KERNEL_NAME, RELEASE);
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        panic("not loaded by a Multiboot bootloader");
    if (mbi->flags & 1)
        mem_upper_kb = mbi->mem_upper;

    gdt_init();
    ok("GDT loaded");
    idt_init();
    ok("IDT loaded");
    pic_init();
    ok("PIC remapped to vectors 32-47");
    timer_init(100);
    ok("PIT timer at 100 Hz");
    keyboard_init();
    ok("PS/2 keyboard");
    mouse_init(GFX_WIDTH, GFX_HEIGHT);
    ok(mouse_has_wheel() ? "PS/2 mouse with wheel" : "PS/2 mouse");
    heap_init(mem_upper_kb);
    fs_init();
    sysfiles_init(mem_upper_kb);
    shell_install();
    ok("RAM disk with the CraftOS layout");
    tasks_init();
    ok("Task scheduler");

    __asm__ volatile("sti");
    ok("Interrupts enabled");
    if (sb16_init())
        ok("SB16 audio");
    else
        kprintf("[FAIL] SB16 audio\n");
    sb16_play_tone(440, 500);
    speaker_beep(440, 200);

    /* dhcp only waits a few seconds at most */
    if (net_init()) {
        ok("RTL8139 network card");
        if (net_dhcp()) {
            char ip[16], line[48];
            net_format_ip(net.ip, ip);
            ksnprintf(line, sizeof(line), "Network address %s (DHCP)", ip);
            ok(line);
        } else {
            kprintf("[FAIL] No DHCP answer; the network is offline (try 'dhcp')\n");
        }
    } else {
        kprintf("[ -- ] No network card (QEMU: -nic user,model=rtl8139)\n");
    }

    kprintf("\n%u MiB of memory available.\n\n", mem_upper_kb / 1024);

    shell_start(&console_shell);
    char line[SHELL_LINE_MAX + 1];
    for (;;) {
        shell_prompt(&console_shell);
        read_line(line, sizeof(line));
        shell_run(&console_shell, line);
    }
}
