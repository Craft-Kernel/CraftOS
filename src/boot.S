# the very first code that runs, straight from the bootloader (multiboot v1).
#
# grub, or `qemu -kernel`, drops us at 1 mib with the cpu already in 32-bit
# protected mode and paging off. eax holds the multiboot magic number,
# which proves a proper bootloader started us, and ebx points at the info
# it filled in for us (like how much memory there is). all we do here is
# give ourselves a stack and pass both of those along to kernel_main.

.set MB_ALIGN,    1 << 0
.set MB_MEMINFO,  1 << 1
.set MB_FLAGS,    MB_ALIGN | MB_MEMINFO
.set MB_MAGIC,    0x1BADB002
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

.section .bss
.align 16
stack_bottom:
.skip 65536
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    mov  $stack_top, %esp

    # c code expects the stack to be lined up on 16 bytes when it's called,
    # so pad it a little before pushing the two arguments.
    sub  $8, %esp
    push %ebx
    push %eax
    call kernel_main

    cli
1:  hlt
    jmp  1b
.size _start, . - _start
