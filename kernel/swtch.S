// Context switch, xv6 style.
//
// void swtch(struct context *old, struct context *new)
//   x0 = save current callee-saved state here
//   x1 = load this thread's state and return as it
//
// Why only x19-x30 + sp? swtch is an ordinary function call, so the
// AAPCS64 ABI already guarantees the *caller* spilled anything live in
// x0-x18 before calling us. Callee-saved registers are the only state
// a function may keep across a call -- so they're the only state a
// thread keeps across a switch. (A thread interrupted by the timer has
// its FULL register set in the trap frame on its own stack; swtch just
// switches which stack -- and therefore which trap frame -- resumes.)
//
// The magic is the last two lines: sp and x30 now belong to the new
// thread, so `ret` returns wherever *it* called swtch from -- or, for
// a brand-new thread, to thread_start below.

.globl swtch
swtch:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x9, sp
    str     x9, [x0, #96]

    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x30, [x1, #80]
    ldr     x9, [x1, #96]
    mov     sp, x9
    ret

// First landing of a new thread. thread_create points the context's
// x30 here and stashes the thread function in x19. We arrive from
// swtch inside the timer IRQ path, so IRQs are masked -- unmask, then
// run the thread. Thread functions must never return.
.globl thread_start
thread_start:
    msr     daifclr, #2
    blr     x19
    bl      thread_return_panic

// One-way door down to EL0.
//
// void enter_user(uint64_t entry, uint64_t user_sp)
//
// eret to EL0: ELR = user entry point, SPSR = 0 (EL0t, all interrupt
// masks clear -- the timer keeps preempting), SP_EL0 = user stack.
// EL0 has its own stack pointer register; ours (SP_EL1) stays put and
// becomes the kernel stack every trap from this program lands on.
.globl enter_user
enter_user:
    msr     sp_el0, x1
    msr     elr_el1, x0
    msr     spsr_el1, xzr
    eret
