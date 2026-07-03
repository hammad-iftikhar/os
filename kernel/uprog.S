// The user program. Lives in the kernel image (there's no filesystem
// to load from), but is completely self-contained and position-
// independent: the kernel memcpy's this section into a fresh page
// mapped at USER_CODE_VA and jumps to it at EL0. It may not call any
// kernel function or touch any kernel address -- the only door is svc.
//
// Syscall ABI: number in x8, args in x0..x5, result in x0.
//   SYS_write = 0  (x0 = pointer to NUL-terminated string)
//   SYS_ticks = 1  (returns tick count)

.section .user_code, "ax"
.globl user_program
user_program:
    mov     x20, #3             // three polite greetings...
1:
    adr     x0, umsg            // PC-relative: works at the user VA
    mov     x8, #0              // SYS_write
    svc     #0

    mov     x9, #0x4000000      // crude delay spanning several timer
2:  subs    x9, x9, #1          // ticks, so K gets scheduled between
    b.ne    2b                  // greetings

    subs    x20, x20, #1
    b.ne    1b

    adr     x0, lastmsg         // ...then announce the crime
    mov     x8, #0
    svc     #0

    mov     x0, #0x40080000     // kernel .text (single movz: 0x4008<<16)
    ldr     x1, [x0]            // hardware says no: data abort, EL0

    // unreachable
3:  b       3b

umsg:    .asciz "[user] hello from EL0 via svc\n"
lastmsg: .asciz "[user] now reading kernel memory at 0x40080000...\n"
