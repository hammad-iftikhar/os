// EL1 exception vector table.
//
// Hardware contract: VBAR_EL1 points at a 2048-byte-aligned table of
// 16 entries, each 128 bytes of code. Which entry runs is picked by
// (where the exception came from) x (what kind it is):
//
//   entries  0-3   current EL, using SP_EL0   (we never run this way)
//   entries  4-7   current EL, using SP_EL1   <- kernel faults land here
//   entries  8-11  lower EL, AArch64          <- user programs, milestone 7
//   entries 12-15  lower EL, AArch32          (we don't support 32-bit)
//   kinds within each group: sync, IRQ, FIQ, SError
//
// Every entry does the same thing: build a trap frame, call C with the
// frame and its own index. 128 bytes = 32 instructions is too tight to
// be comfortable, so entries save x0/x1, load their index, and branch
// to a shared stub for the rest.

.macro ventry idx
.balign 128
    sub     sp, sp, #288            // room for the trap frame
    stp     x0, x1, [sp, #0]        // free up two scratch registers
    mov     x0, #\idx
    b       save_rest
.endm

.balign 2048
.globl vectors
vectors:
    ventry  0
    ventry  1
    ventry  2
    ventry  3
    ventry  4
    ventry  5
    ventry  6
    ventry  7
    ventry  8
    ventry  9
    ventry  10
    ventry  11
    ventry  12
    ventry  13
    ventry  14
    ventry  15

save_rest:
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

    mov     x1, x0                  // arg1 = vector index
    mov     x0, sp                  // arg0 = trap frame
    bl      handle_exception
    b       .                       // handler panics; never returns
