#include <cpu.h>
#include <io.h>

/*
 * the pc has two interrupt controller chips (8259 pics) chained together. out
 * of the box they deliver hardware interrupts 0-15 on vectors 8-15 and
 * 0x70-0x77, and the first lot clashes with the cpu's own exceptions. the
 * four setup words below (icw1 to icw4) move them up to 32-47, with the
 * second chip plugged into the first one's line 2.
 *
 * every line starts switched off; each driver turns on its own with
 * irq_register().
 */

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

void pic_init(void)
{
    outb(PIC1_CMD, 0x11);  io_wait();
    outb(PIC2_CMD, 0x11);  io_wait();
    outb(PIC1_DATA, 32);   io_wait();
    outb(PIC2_DATA, 40);   io_wait();
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/*
 * a line on the second chip only gets through if line 2 on the first chip
 * (where the second one plugs in) is open as well.
 */
void pic_unmask(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_DATA, inb(PIC2_DATA) & ~(1 << (irq - 8)));
        irq = 2;
    }
    outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << irq));
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}
