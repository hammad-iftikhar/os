// EL1 exception vector table.
//
// Hardware contract: VBAR_EL1 points at a 2048-byte-aligned table of
// 16 entries, each 128 bytes of code. Which entry runs is picked by
// (where the exception came from) x (what kind it is):
//
//   entries  0-3   current EL, using SP_EL0   (we never run this way)
//   entries  4-7   current EL, using SP_EL1   <- kernel faults + IRQs
//   entries  8-11  lower EL, AArch64          <- user programs, milestone 7
//   entries 12-15  lower EL, AArch32          (we don't support 32-bit)
//   kinds within each group: sync, IRQ, FIQ, SError
//
// Two stubs share the same trap-frame layout (contract with trap.c):
//   fatal_stub: save everything, call handle_exception -> panics
//   irq_stub:   save everything, call handle_irq, restore, eret

.macro ventry idx, stub
.balign 128
    sub     sp, sp, #288            // room for the trap frame
    stp     x0, x1, [sp, #0]        // free up two scratch registers
    mov     x0, #\idx
    b       \stub
.endm

.macro save_rest                    // everything except x0/x1 (already saved)
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x1, elr_el1
    stp     x30, x1, [sp, #240]
    mrs     x1, spsr_el1
    mrs     x2, esr_el1
    stp     x1, x2, [sp, #256]
    mrs     x1, far_el1
    str     x1, [sp, #272]
.endm

.balign 2048
.globl vectors
vectors:
    ventry  0, fatal_stub
    ventry  1, fatal_stub
    ventry  2, fatal_stub
    ventry  3, fatal_stub
    ventry  4, fatal_stub
    ventry  5, irq_stub             // IRQ at EL1h: the survivable one
    ventry  6, fatal_stub
    ventry  7, fatal_stub
    ventry  8, sync_el0_stub        // sync from EL0: svc or user fault
    ventry  9, irq_stub             // timer preempts user code too
    ventry  10, fatal_stub
    ventry  11, fatal_stub
    ventry  12, fatal_stub
    ventry  13, fatal_stub
    ventry  14, fatal_stub
    ventry  15, fatal_stub

fatal_stub:
    save_rest
    mov     x1, x0                  // arg1 = vector index
    mov     x0, sp                  // arg0 = trap frame
    bl      handle_exception
    b       .                       // handler panics; never returns

irq_stub:
    save_rest
    mov     x0, sp
    bl      handle_irq
    b       restore_and_eret

// Sync exception from EL0 (vector 8): an svc syscall, or a user fault.
// handle_sync_el0 returns for syscalls (restore + eret resumes the
// user program right after its svc); user faults never come back.
sync_el0_stub:
    save_rest
    mov     x0, sp
    bl      handle_sync_el0
    b       restore_and_eret

restore_and_eret:
    // Undo everything: ELR/SPSR first (they need scratch registers),
    // then the GPRs, x0/x1 last, and eret jumps back to ELR.
    ldp     x1, x2, [sp, #248]      // elr, spsr
    msr     elr_el1, x1
    msr     spsr_el1, x2
    ldr     x30, [sp, #240]
    ldp     x28, x29, [sp, #224]
    ldp     x26, x27, [sp, #208]
    ldp     x24, x25, [sp, #192]
    ldp     x22, x23, [sp, #176]
    ldp     x20, x21, [sp, #160]
    ldp     x18, x19, [sp, #144]
    ldp     x16, x17, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96]
    ldp     x10, x11, [sp, #80]
    ldp     x8, x9, [sp, #64]
    ldp     x6, x7, [sp, #48]
    ldp     x4, x5, [sp, #32]
    ldp     x2, x3, [sp, #16]
    ldp     x0, x1, [sp, #0]
    add     sp, sp, #288
    eret
