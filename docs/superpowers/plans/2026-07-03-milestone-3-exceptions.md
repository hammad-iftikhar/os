# Milestone 3: Exceptions — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Install an EL1 exception vector table so a crashing kernel prints a full register dump and panics instead of hanging silently.

**Architecture:** `kernel/vectors.S` defines the 2048-byte-aligned vector table (16 entries × 128 bytes). Every entry saves x0–x30 + ELR/SPSR/ESR/FAR into a trap frame on the stack and calls one C handler, `handle_exception(frame, vector_index)`, in `kernel/trap.c`, which decodes ESR_EL1, dumps all registers, and panics. `exceptions_init()` writes the table address to `VBAR_EL1`. `kmain` demos it with a deliberate unaligned read — **verified on this machine**: with the MMU off all memory is Device-type, so an unaligned read faults (currently a silent hang).

**Tech Stack:** Same as milestones 1–2. New: AArch64 system registers `VBAR_EL1`, `ESR_EL1`, `ELR_EL1`, `SPSR_EL1`, `FAR_EL1`; GNU assembler macros.

## Global Constraints

- Toolchain: `clang --target=aarch64-elf -ffreestanding -mgeneral-regs-only`; link with `ld.lld -T linker.ld` (no libc).
- Target: `qemu-system-aarch64 -machine virt -cpu cortex-a72`.
- Error philosophy: panic loudly; never limp along (from spec).
- Commits: author Hammad Iftikhar <hammad.iftikhar723@gmail.com>; NO Co-Authored-By trailer.
- Build must stay warning-free under `-Wall -Wextra`.
- No restore/return path in the handler yet — milestone 3 exceptions are always fatal. The restore path arrives in milestone 4 when IRQs make exceptions survivable. (Deliberate scope cut, not an omission.)

## File Structure

```
kernel/
├── vectors.S    # NEW: vector table + register-save stub
├── trap.h       # NEW: exceptions_init() declaration
├── trap.c       # NEW: trap_frame struct, ESR decode, handler, exceptions_init
├── main.c       # MODIFIED: install vectors, deliberate unaligned read
└── (uart/kprintf unchanged)
Makefile         # MODIFIED: vectors.o, trap.o, kernel/*.S rule, trap.h dep
```

**Trap frame layout contract** (asm writes it, C reads it — offsets must match exactly):

| Offset | Contents |
|---|---|
| 0–247 | `x0`..`x30` (8 bytes each) |
| 248 | `ELR_EL1` (where the exception happened) |
| 256 | `SPSR_EL1` (saved processor state) |
| 264 | `ESR_EL1` (exception syndrome — what happened) |
| 272 | `FAR_EL1` (faulting address, for aborts) |
| 280–287 | padding (SP must stay 16-byte aligned → frame is 288 bytes) |

---

### Task 1: Vector table, trap handler, crash demo

One deliverable: the crash dump. Table, stub, and handler are inseparable halves of it.

**Files:**
- Create: `kernel/vectors.S`
- Create: `kernel/trap.h`
- Create: `kernel/trap.c`
- Modify: `kernel/main.c` (replace whole file)
- Modify: `Makefile`
- Test: observable QEMU serial output (per spec)

**Interfaces:**
- Consumes: `kprintf`, `panic` from `kernel/kprintf.h` (milestone 2).
- Produces: `void exceptions_init(void)` (called by `kmain`, and by all later milestones at boot); `struct trap_frame` in `trap.c` (milestone 4's IRQ path will extend this file); asm symbol `vectors` (the table).

- [ ] **Step 1: Write `kernel/vectors.S`**

```asm
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
```

- [ ] **Step 2: Write `kernel/trap.h`**

```c
#pragma once

void exceptions_init(void);
```

- [ ] **Step 3: Write `kernel/trap.c`**

```c
#include <stdint.h>
#include "kprintf.h"
#include "trap.h"

// Filled in by save_rest in vectors.S. Field order and offsets are a
// contract with that code -- do not reorder.
struct trap_frame {
    uint64_t x[31];     // x0..x30
    uint64_t elr;       // where the exception happened
    uint64_t spsr;      // saved processor state
    uint64_t esr;       // exception syndrome: what and why
    uint64_t far;       // faulting address (valid for aborts)
};

static const char *vector_names[16] = {
    "sync EL1t", "irq EL1t", "fiq EL1t", "serror EL1t",
    "sync EL1h", "irq EL1h", "fiq EL1h", "serror EL1h",
    "sync EL0/64", "irq EL0/64", "fiq EL0/64", "serror EL0/64",
    "sync EL0/32", "irq EL0/32", "fiq EL0/32", "serror EL0/32",
};

// ESR_EL1[31:26] = exception class. The few we can hit now.
static const char *ec_name(uint32_t ec)
{
    switch (ec) {
    case 0x15: return "SVC instruction";
    case 0x20: return "instruction abort, lower EL";
    case 0x21: return "instruction abort, same EL";
    case 0x22: return "PC alignment fault";
    case 0x24: return "data abort, lower EL";
    case 0x25: return "data abort, same EL";
    case 0x26: return "SP alignment fault";
    case 0x3c: return "BRK instruction";
    default:   return "unknown";
    }
}

void handle_exception(struct trap_frame *tf, uint64_t vector);

void handle_exception(struct trap_frame *tf, uint64_t vector)
{
    uint32_t ec = (uint32_t)(tf->esr >> 26) & 0x3f;

    kprintf("\n*** EXCEPTION: %s ***\n", vector_names[vector & 0xf]);
    kprintf("ESR  = %p  EC=%x: %s\n", (void *)tf->esr, ec, ec_name(ec));
    kprintf("ELR  = %p  (faulting instruction)\n", (void *)tf->elr);
    kprintf("FAR  = %p  (faulting address)\n", (void *)tf->far);
    kprintf("SPSR = %p\n", (void *)tf->spsr);

    for (int i = 0; i < 31; i += 2) {
        kprintf("x%d%s = %p", i, i < 10 ? " " : "", (void *)tf->x[i]);
        if (i + 1 < 31)
            kprintf("   x%d%s = %p", i + 1, i + 1 < 10 ? " " : "",
                    (void *)tf->x[i + 1]);
        kprintf("\n");
    }

    panic("unhandled exception");
}

void exceptions_init(void)
{
    extern char vectors[];

    __asm__ volatile("msr vbar_el1, %0" :: "r"(vectors));
}
```

- [ ] **Step 4: Replace `kernel/main.c`**

```c
#include "kprintf.h"
#include "trap.h"

void kmain(void)
{
    exceptions_init();
    kprintf("Hello from kernel\n");
    kprintf("vectors installed; now crashing on purpose...\n");

    // With the MMU off, all memory is Device-type and unaligned
    // access faults: data abort, FAR = the bad address.
    volatile unsigned int *p = (unsigned int *)0x40080001UL;
    kprintf("read: %x\n", *p);

    panic("unreachable: the read above must fault");
}
```

- [ ] **Step 5: Update the Makefile**

`OBJS` line:

```make
OBJS := build/boot.o build/main.o build/uart.o build/kprintf.o \
        build/vectors.o build/trap.o
```

Header dependency line (trap.h joins):

```make
build/%.o: kernel/%.c kernel/uart.h kernel/kprintf.h kernel/trap.h | build
	$(CC) $(CFLAGS) -c $< -o $@
```

New rule for assembly inside `kernel/` (after the `build/boot.o` rule):

```make
build/%.o: kernel/%.S | build
	$(CC) $(CROSS) -c $< -o $@
```

- [ ] **Step 6: Build**

Run: `make`
Expected: no warnings; links `build/kernel.elf`.

- [ ] **Step 7: Scripted output check**

```bash
qemu-system-aarch64 -machine virt -cpu cortex-a72 -display none \
    -serial file:/tmp/os-serial.log -kernel build/kernel.elf & QPID=$!
sleep 3; kill $QPID
cat /tmp/os-serial.log
```

Expected (exact register values vary; the load-bearing lines):

```
Hello from kernel
vectors installed; now crashing on purpose...

*** EXCEPTION: sync EL1h ***
ESR  = 0x0000000096000021  EC=25: data abort, same EL
ELR  = 0x00000000400800XX  (faulting instruction)
FAR  = 0x0000000040080001  (faulting address)
...
x0  = ...   x1  = ...
[16 rows of registers]

PANIC: unhandled exception
```

Must-check: vector name is `sync EL1h`, EC is `25` (data abort, same EL), FAR is exactly `0x0000000040080001`, and the final line is the panic.

- [ ] **Step 8: Commit**

```bash
git add kernel/vectors.S kernel/trap.h kernel/trap.c kernel/main.c Makefile
git commit -m "milestone 3: EL1 vector table, register-dump exception handler"
```

---

### Task 2: Milestone explainer doc

**Files:**
- Create: `docs/03-exceptions.md`

**Interfaces:**
- Consumes: the code from Task 1.
- Produces: learning artifact in the established `docs/NN-topic.md` format.

- [ ] **Step 1: Write `docs/03-exceptions.md`**

Cover exactly these sections, expanded into teaching prose with code from Task 1 inlined:

1. **What an exception is** — the CPU's only mechanism for "stop what you're doing": faults (bad memory access), traps (`svc`, `brk`), and interrupts (hardware, milestone 4) all funnel through the same machinery. Before this milestone, our fault "handler" was whatever garbage VBAR_EL1 pointed at — hence the silent hang we demonstrated.
2. **The vector table shape** — 16 entries of 128 bytes each, 2048-byte aligned; the 4×4 grid (came-from × kind) with the table from vectors.S; why kernel faults land in "current EL, SP_EL1" (EL1h); entries are *code*, not addresses — the CPU literally jumps into the table.
3. **What the hardware does for us at exception entry** — automatically: saves PC to `ELR_EL1`, state to `SPSR_EL1`, cause to `ESR_EL1`, faulting address to `FAR_EL1`, masks interrupts, jumps to the right vector. Everything else — the 31 general registers — is *our* job to save.
4. **The trap frame** — why save x0–x30 (the handler's own C code would clobber them; for a register dump we need them pristine); the x0/x1-first-then-branch trick to fit in 128 bytes; the offset table as a hard contract between `vectors.S` and `struct trap_frame`; why the frame is 288 bytes (16-byte SP alignment, again).
5. **Reading ESR_EL1** — bits [31:26] = exception class (EC): the decode table; syndrome bits below hold detail per class. Walk the demo's actual value `0x96000021`: EC=0x25 data abort same EL, and the low bits encoding "alignment fault" (DFSC=0b100001).
6. **Why the unaligned read faults** — with MMU off, every address is Device-nGnRnE memory, which architecturally requires aligned access. Empirically verified before this milestone (the silent hang). Once the MMU is on (milestone 5) and RAM is marked Normal memory, unaligned access becomes legal — this crash *will stop crashing*.
7. **Try breaking it** — 3 experiments: change the deref to `svc #0` via `__asm__ volatile("svc #0")` and watch EC become 0x15; try `brk #0` (EC 0x3c); comment out `exceptions_init()` and confirm the silent hang returns.

- [ ] **Step 2: Commit**

```bash
git add docs/03-exceptions.md
git commit -m "docs: milestone 3 explainer — vector table, trap frames, ESR"
```

---

## Self-review notes

- Spec coverage: milestone 3 = "EL1 vector table, sync exception handler with register dump; deliberately crash and see it caught" — Task 1 delivers all three; Task 2 the doc.
- The crash trigger (unaligned read with MMU off) was verified empirically on this machine before writing this plan.
- Type consistency: `handle_exception(struct trap_frame *, uint64_t)` matches the asm call (x0 = sp = frame, x1 = index); frame offsets match the struct field order.
- No restore path: named as a deliberate scope cut in Global Constraints, arrives in milestone 4.
