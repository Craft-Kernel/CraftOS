/*
 * the kernel is loaded at 1 mib, the first address above the bios and
 * legacy video areas. the multiboot header has to land in the first
 * 8 kib of the image, so it goes at the very start of .text.
 */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text*)
    }

    .rodata BLOCK(4K) : ALIGN(4K) { *(.rodata*) }
    .data   BLOCK(4K) : ALIGN(4K) { *(.data*) }
    .bss    BLOCK(4K) : ALIGN(4K) { *(COMMON) *(.bss*) }

    kernel_end = .;

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
