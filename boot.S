// Entry point. QEMU's -kernel loads this ELF and starts the boot CPU
// here at EL1, MMU off, caches off, x0 = physical address of the DTB.

.section .text.boot
.globl _start
_start:
    // Park every core except core 0 (harmless with -smp 1,
    // required the day we pass -smp 4).
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff           // Aff0 = core number within cluster
    cbnz    x0, halt

    // Clear BSS: C code assumes zeroed globals.
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b

2:  // C needs a stack.
    ldr     x0, =__stack_top
    mov     sp, x0
    bl      kmain

halt:                               // if kmain returns, or non-boot core
    wfe
    b       halt
