#include <cpu.h>
#include <io.h>

/*
 * to restart, we pulse the cpu's reset line through the keyboard controller.
 * it's the oldest reboot trick on the pc and it still works everywhere.
 */
void machine_reboot(void)
{
    while (inb(0x64) & 2)
        ;
    outb(0x64, 0xFE);
    halt_forever();
}

/*
 * switching the power off properly needs acpi, which we don't have yet.
 * emulators have a shortcut, though: writing to their power management port.
 * these are the ports for current qemu, older qemu and bochs, and virtualbox.
 * on real hardware none of them do anything, and we just come back.
 */
void machine_poweroff(void)
{
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
}
