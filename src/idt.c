#include <console.h>
#include <cpu.h>
#include <gfx.h>
#include <io.h>
#include <string.h>
#include <task.h>

/*
 * the interrupt table: one entry for every way the cpu can get interrupted.
 * the first 32 are the cpu's own complaints (divide by zero, page fault and
 * so on), 32-47 are hardware like the timer and keyboard coming through the
 * pic, and 48 is how a task politely gives up its turn. every entry points at
 * a tiny assembly stub, and all the stubs end up in interrupt_handler()
 * further down.
 */

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

#define IDT_VECTORS 49

static struct idt_entry idt[256];
static struct idt_ptr idtr;
static irq_handler_t irq_handlers[16];

extern const uint32_t isr_stub_table[IDT_VECTORS];
extern void idt_load(struct idt_ptr *ptr);

static const char *const exception_names[32] = {
    "Divide by zero", "Debug", "Non-maskable interrupt", "Breakpoint",
    "Overflow", "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS", "Segment not present",
    "Stack-segment fault", "General protection fault", "Page fault", "Reserved",
    "x87 floating-point exception", "Alignment check", "Machine check", "SIMD floating-point exception",
    "Virtualization exception", "Control protection exception", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection exception", "VMM communication exception", "Security exception", "Reserved",
};

/*
 * point every entry at its stub, in the kernel's code segment. the 0x8e flag
 * means "this entry is real, kernel only, and a 32-bit interrupt gate", which
 * also keeps interrupts switched off while the handler runs.
 */
void idt_init(void)
{
    for (int i = 0; i < IDT_VECTORS; i++) {
        uint32_t addr = isr_stub_table[i];
        idt[i].offset_low = addr & 0xFFFF;
        idt[i].offset_high = addr >> 16;
        idt[i].selector = 0x08;
        idt[i].zero = 0;
        idt[i].type_attr = 0x8E;
    }
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint32_t)&idt;
    idt_load(&idtr);
}

void irq_register(uint8_t irq, irq_handler_t handler)
{
    irq_handlers[irq] = handler;
    pic_unmask(irq);
}

/*
 * the panic screen. the whole text screen turns red, with the craft kernel
 * banner and the reason on it, and then the cpu stops for good. if the
 * desktop was up, it switches back to text mode first so you can actually see
 * it. a cpu exception also gets its registers listed under the banner, since
 * those are what tell you where it went wrong.
 */
static void __attribute__((noreturn)) panic_screen(const char *reason, struct regs *r)
{
    __asm__ volatile("cli");
    if (gfx_active())
        gfx_exit();
    console_set_color(VGA_WHITE, VGA_RED);
    console_clear();

    kprintf("-------------- Craft Kernel --------------\n\n");
    kprintf("               KERNEL PANIC!\n\n");
    kprintf("        the kernel fucked off sorry\n\n");
    kprintf("      %s\n\n", reason);
    kprintf("-------------- Craft Kernel --------------\n\n\n");

    if (r) {
        kprintf("eip=%p  cs=%04x  eflags=%08x  err=%08x\n", (void *)r->eip, r->cs, r->eflags, r->err_code);
        kprintf("eax=%08x ebx=%08x ecx=%08x edx=%08x\n", r->eax, r->ebx, r->ecx, r->edx);
        kprintf("esi=%08x edi=%08x ebp=%08x\n", r->esi, r->edi, r->ebp);
        if (r->int_no == 14) {
            uint32_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            kprintf("faulting address (cr2)=%08x\n", cr2);
        }
    }
    halt_forever();
}

/*
 * there's no way to recover from a cpu exception yet, so every one of them
 * ends up on the panic screen.
 */
static void exception(struct regs *r)
{
    char reason[64];
    ksnprintf(reason, sizeof(reason), "cpu exception %u: %s", r->int_no, exception_names[r->int_no]);
    panic_screen(reason, r);
}

/*
 * every interrupt comes through here. it returns the stack to carry on with,
 * which is how task switching works: on a timer tick, or when a task gives up
 * its turn, the scheduler can hand back a different task's stack. any other
 * hardware interrupt just wakes up the tasks that were waiting for something
 * to happen.
 */
uint32_t interrupt_handler(struct regs *r)
{
    if (r->int_no < 32)
        exception(r);

    if (r->int_no == TASK_YIELD_VECTOR)
        return task_schedule((uint32_t)r, false);

    uint8_t irq = r->int_no - 32;
    if (irq_handlers[irq])
        irq_handlers[irq](r);
    pic_send_eoi(irq);

    task_wake_waiting();
    if (irq == 0)
        return task_schedule((uint32_t)r, true);
    return (uint32_t)r;
}

void panic(const char *msg)
{
    panic_screen(msg, 0);
}
