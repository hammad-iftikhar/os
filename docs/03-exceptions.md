# Milestone 3: Exceptions

Before this milestone, a kernel bug meant *silence*: the demo unaligned read just hung, no message, no clue. Now the same bug prints which instruction faulted, what address it touched, why the CPU objected, and every register at the moment of impact. This is the difference between debugging and guessing.

## 1. What an exception is

An **exception** is the CPU's only mechanism for "stop what you're doing and run this other code instead." Everything funnels through it:

- **Faults** — the program did something impossible: touched an unmapped address, executed garbage, misaligned an access.
- **Traps** — the program *asked* to enter the kernel: `svc` (syscalls, milestone 7), `brk` (breakpoints).
- **Interrupts** — hardware wants attention: timer, devices (milestone 4).

When an exception fires, the CPU jumps to a location derived from `VBAR_EL1` — the *Vector Base Address Register*. We had never set it. Whatever garbage it held at reset, the CPU jumped there, executed nonsense or nothing, and hung. That was our silent hang. `exceptions_init()` is one instruction of fix:

```c
__asm__ volatile("msr vbar_el1, %0" :: "r"(vectors));
```

## 2. The vector table shape

The table is rigidly shaped by hardware: **16 entries × 128 bytes, 2048-byte aligned**. The CPU picks an entry by a 4×4 grid — *where the exception came from* × *what kind it is*:

| | sync | IRQ | FIQ | SError |
|---|---|---|---|---|
| **current EL, SP_EL0** (EL1t) | 0 | 1 | 2 | 3 |
| **current EL, SP_EL1** (EL1h) | 4 | 5 | 6 | 7 |
| **lower EL, AArch64** | 8 | 9 | 10 | 11 |
| **lower EL, AArch32** | 12 | 13 | 14 | 15 |

Our kernel runs at EL1 on SP_EL1, so kernel faults land in row two — the demo's dump says `sync EL1h`, entry 4. Row three is where user-program syscalls will arrive in milestone 7. Rows one and four we never use (we don't run on SP_EL0, and we don't support 32-bit).

The subtle part: entries are **code, not addresses**. The CPU doesn't read a pointer from the table — it *jumps into* the table and starts executing. Each entry is a 128-byte slot of instructions:

```asm
.macro ventry idx
.balign 128
    sub     sp, sp, #288            // room for the trap frame
    stp     x0, x1, [sp, #0]        // free up two scratch registers
    mov     x0, #\idx
    b       save_rest
.endm
```

128 bytes is 32 instructions — too tight for the full register save plus a call. So each entry saves just x0/x1 (freeing two registers to work with), records its own index, and branches to a shared stub. All 16 entries funnel into one `save_rest`.

## 3. What the hardware does for us

At exception entry, before executing a single instruction of ours, the CPU has already:

1. Saved the interrupted PC into **`ELR_EL1`** (Exception Link Register) — where to resume.
2. Saved processor state (flags, interrupt masks, which EL) into **`SPSR_EL1`**.
3. Written *why* into **`ESR_EL1`** (Exception Syndrome Register).
4. For memory faults, written the offending address into **`FAR_EL1`** (Fault Address Register).
5. Masked interrupts, switched to EL1, jumped to the vector.

What it has *not* done: saved x0–x30. That's our job, and it's the whole reason `vectors.S` exists.

## 4. The trap frame

The moment we call any C code, the compiler will freely clobber registers — and for a crash dump, we need them exactly as they were at the instant of the fault. So the stub stores everything into a **trap frame** on the stack before touching C:

| Offset | Contents |
|---|---|
| 0–247 | x0..x30 |
| 248 | ELR_EL1 |
| 256 | SPSR_EL1 |
| 264 | ESR_EL1 |
| 272 | FAR_EL1 |
| 280–287 | padding → 288 bytes total (SP must stay 16-byte aligned) |

On the C side this layout reappears as a struct — field order is a **contract** between the two files:

```c
struct trap_frame {
    uint64_t x[31];
    uint64_t elr, spsr, esr, far;
};
```

Then the handoff to C is two movs: `x0` = frame pointer, `x1` = vector index, `bl handle_exception`. The dump you saw is just C reading that struct.

There is deliberately **no restore path** yet — every exception ends in `panic`. Milestone 4 adds the mirror-image register restore and `eret` when interrupts make exceptions something you *survive*.

## 5. Reading ESR_EL1

The syndrome register packs "what happened" into bitfields. Bits **[31:26]** are the **exception class (EC)** — the coarse category. Our decoder handles the ones we can currently hit (SVC, aborts, alignment, BRK).

Walk the demo's actual value, `ESR = 0x96000021`:

```
0x96000021 = 1001 0110 0000 ... 0010 0001
             ^^^^ ^^
             EC = 0b100101 = 0x25  → data abort, same EL
                                  low bits: DFSC = 0b100001 = alignment fault
```

The low 6 bits (DFSC, *Data Fault Status Code*) say precisely: alignment. Combined with `FAR = 0x40080001` (the odd address) and `ELR = 0x40080084` (the load instruction), the CPU has essentially filed a complete bug report. Look at the dump again: `x8 = 0x40080001` — you can see the bad pointer sitting in the register the compiler chose for the dereference.

## 6. Why the unaligned read faults (and why it will stop faulting)

With the MMU off, the architecture treats **all** memory as Device-nGnRnE — the strictest device type, meant for hardware registers where a stray or split access could have side effects. Device memory architecturally requires aligned accesses; our 4-byte read at `...01` violated that. We verified this behavior empirically before writing the milestone (the silent hang).

Here's the twist worth remembering: in milestone 5 we turn the MMU on and mark RAM as *Normal* memory — where unaligned access is perfectly legal and handled by hardware. **This exact crash will stop crashing.** Same code, different memory attributes, different outcome. Keep it in mind as a preview of how much the MMU changes the rules.

## 7. Try breaking it

1. **`svc #0`** — replace the unaligned read with `__asm__ volatile("svc #0");`. EC becomes 0x15 "SVC instruction". You've just made your first syscall — there's simply no syscall table yet, so it panics. Milestone 7 turns this exact path into a real kernel API.
2. **`brk #0`** — same but EC 0x3c. This is what debugger breakpoints compile to.
3. **Comment out `exceptions_init()`** in `kmain` and run again. Silence. One `msr` instruction is all that separates a debuggable kernel from a brick.

## Where we go next

Milestone 4: so far, exceptions only report disasters. Next we make them *routine*: the GIC (interrupt controller) plus the ARM generic timer, so the kernel gets a periodic tick — and we add the register-restore path so the interrupted code resumes as if nothing happened. That tick becomes the scheduler's heartbeat in milestone 6.
