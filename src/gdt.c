#include <cpu.h>

/*
 * x86 won't let you switch memory segments off completely, so we do the next
 * best thing: set up one code segment and one data segment that both start at
 * address 0 and cover all 4 gib. after that, every address just means what it
 * says, and we can forget segments exist.
 *
 * selector 0x08 picks the code segment and 0x10 the data segment.
 */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt[3];
static struct gdt_ptr gdtr;

extern void gdt_flush(struct gdt_ptr *ptr);

static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt[i].base_low = base & 0xFFFF;
    gdt[i].base_mid = (base >> 16) & 0xFF;
    gdt[i].base_high = (base >> 24) & 0xFF;
    gdt[i].limit_low = limit & 0xFFFF;
    gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access = access;
}

void gdt_init(void)
{
    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    gdt_set(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (uint32_t)&gdt;
    gdt_flush(&gdtr);
}
