# the bits of cpu plumbing that have to be written in assembly: loading the
# segment and interrupt tables, and the little entry stubs every interrupt
# goes through on its way to the c code.

.section .text

# a new segment table doesn't do anything until the segment registers are
# loaded again. cs can't be set directly, so a far jump to the very next
# instruction does it for us.
.global gdt_flush
gdt_flush:
    mov  4(%esp), %eax
    lgdt (%eax)
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %fs
    mov  %ax, %gs
    mov  %ax, %ss
    ljmp $0x08, $1f
1:  ret

.global idt_load
idt_load:
    mov  4(%esp), %eax
    lidt (%eax)
    ret

# every stub leaves the stack looking the same: an error code, then which
# interrupt this is. the cpu only pushes an error code for some exceptions,
# so the others push a pretend 0, and the c code never has to care which
# kind it was.

.macro ISR_NOERR n
isr\n:
    push $0
    push $\n
    jmp  isr_common
.endm

.macro ISR_ERR n
isr\n:
    push $\n
    jmp  isr_common
.endm

.macro IRQ n
irq\n:
    push $0
    push $(32 + \n)
    jmp  isr_common
.endm

# the cpu's own exceptions, 0-31. only 8, 10-14, 17, 21, 29 and 30 come with
# an error code.
.irp n, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31
ISR_NOERR \n
.endr
.irp n, 8,10,11,12,13,14,17,21,29,30
ISR_ERR \n
.endr

# the hardware interrupts 0-15, which the pic hands us as 32-47.
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
IRQ \n
.endr

# interrupt 48 isn't hardware at all. a task raises it itself (`int $48`)
# when it wants to give up the rest of its turn (see task_yield), so it gets
# the same treatment as everything else.
ISR_NOERR 48

# where every stub ends up. it saves all the registers, switches to the
# kernel's data segments, and calls the c code with a pointer to everything
# it saved (that's struct regs in cpu.h).
#
# interrupt_handler hands back the stack to carry on with. usually it's the
# same one we came in on, but when the scheduler decides to switch tasks,
# it's a different task's saved stack instead, so the pops and the iret
# below quietly carry on as that task.
isr_common:
    pusha
    push %ds
    push %es
    push %fs
    push %gs
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %fs
    mov  %ax, %gs
    cld

    push %esp
    call interrupt_handler
    mov  %eax, %esp

    pop  %gs
    pop  %fs
    pop  %es
    pop  %ds
    popa
    add  $8, %esp
    iret

# a list of where every stub is, in interrupt order, for idt_init() to read.
.section .rodata
.global isr_stub_table
isr_stub_table:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .long isr\n
.endr
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    .long irq\n
.endr
    .long isr48
