# Milestone 4: Interrupts & Timer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The kernel gets a heartbeat — GICv2 initialized, ARM generic timer firing once per second, each tick printed, and the interrupted code resuming via a full register-restore + `eret` path.

**Architecture:** `vectors.S` grows a second stub: fatal exceptions keep the old save-and-panic path, but the `irq EL1h` entry (index 5) now saves the trap frame, calls `handle_irq`, restores every register, and `eret`s back to the interrupted instruction. `kernel/gic.c` drives the GICv2 (enable distributor + CPU interface, ack/EOI). `kernel/timer.c` programs the EL1 physical timer (PPI, INTID 30) using `CNTFRQ_EL0`/`CNTP_TVAL_EL0`/`CNTP_CTL_EL0` and re-arms it on each tick. `kmain` unmasks IRQs (`msr daifclr, #2`) and idles in `wfi`.

**Tech Stack:** Same toolchain as milestones 1–3. New hardware: GICv2 — **verified from QEMU's own DTB on this machine**: `arm,cortex-a15-gic`, distributor `0x0800_0000`, CPU interface `0x0801_0000`. ARM generic timer system registers.

## Global Constraints

- Toolchain: `clang --target=aarch64-elf -ffreestanding -mgeneral-regs-only`; link with `ld.lld -T linker.ld` (no libc).
- Target: `qemu-system-aarch64 -machine virt -cpu cortex-a72`.
- Error philosophy: panic loudly; never limp along. Unexpected INTIDs panic; spurious (>=1020) are silently tolerated per GIC spec.
- Commits: author Hammad Iftikhar <hammad.iftikhar723@gmail.com>; NO Co-Authored-By trailer.
- Build must stay warning-free under `-Wall -Wextra`.
- Trap frame layout (offsets 0–287, from milestone 3) is unchanged — the restore path reads the same contract.

## File Structure

```
kernel/
├── vectors.S    # MODIFIED: ventry takes a stub arg; new irq_stub with restore+eret
├── gic.h        # NEW: gic_init, gic_ack, gic_eoi
├── gic.c        # NEW: GICv2 distributor + CPU interface driver
├── timer.h      # NEW: timer_init, timer_tick, TIMER_INTID
├── timer.c      # NEW: EL1 physical timer programming + tick handler
├── trap.c       # MODIFIED: add handle_irq (ack → dispatch → EOI)
├── main.c       # MODIFIED: init everything, unmask IRQs, wfi loop
Makefile         # MODIFIED: gic.o, timer.o, header deps
```

---

### Task 1: IRQ path — GIC driver, timer, restore/eret

One deliverable: the visible heartbeat. The GIC, timer, and restore path only prove themselves together (a tick that prints once but never returns would hang; a perfect restore with no timer never runs).

**Files:**
- Create: `kernel/gic.h`, `kernel/gic.c`, `kernel/timer.h`, `kernel/timer.c`
- Modify: `kernel/vectors.S`, `kernel/trap.c`, `kernel/main.c`, `Makefile`
- Test: observable QEMU serial output (per spec)

**Interfaces:**
- Consumes: `kprintf`/`panic` (milestone 2); trap frame layout and `exceptions_init` (milestone 3).
- Produces: `void gic_init(void)`, `uint32_t gic_ack(void)`, `void gic_eoi(uint32_t intid)`, `void gic_enable_intid(uint32_t intid)`; `void timer_init(uint32_t hz)`, `void timer_tick(void)`, `#define TIMER_INTID 30`; asm `irq_stub` restore path (milestone 6's context switch builds on this frame discipline).

- [ ] **Step 1: Rewrite `kernel/vectors.S`**

Replace the whole file (the save code is unchanged; `ventry` gains a stub parameter and the IRQ path gains restore + eret):

```asm
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
    ventry  8, fatal_stub
    ventry  9, fatal_stub
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
```

Note the offset trick: ELR sits at 248 and SPSR at 256, adjacent — one `ldp` fetches both.

- [ ] **Step 2: Write `kernel/gic.h`**

```c
#pragma once

#include <stdint.h>

void gic_init(void);
void gic_enable_intid(uint32_t intid);
uint32_t gic_ack(void);             // returns INTID (>=1020 = spurious)
void gic_eoi(uint32_t intid);
```

- [ ] **Step 3: Write `kernel/gic.c`**

```c
#include <stdint.h>
#include "gic.h"

// GICv2 on QEMU virt (addresses read from QEMU's own device tree):
// distributor at 0x08000000 (global routing), one CPU interface at
// 0x08010000 (per-core delivery, priority masking, ack/EOI).
#define GICD_BASE       0x08000000UL
#define GICC_BASE       0x08010000UL

#define GICD_CTLR       (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER  ((volatile uint32_t *)(GICD_BASE + 0x100))

#define GICC_CTLR       (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR        (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR        (*(volatile uint32_t *)(GICC_BASE + 0x00c))
#define GICC_EOIR       (*(volatile uint32_t *)(GICC_BASE + 0x010))

void gic_init(void)
{
    GICD_CTLR = 1;      // distributor: start forwarding interrupts
    GICC_PMR = 0xff;    // priority mask: let everything through
    GICC_CTLR = 1;      // CPU interface: start signaling this core
}

void gic_enable_intid(uint32_t intid)
{
    // One bit per interrupt, 32 per register. Write-1-to-enable:
    // no read-modify-write needed, so no race.
    GICD_ISENABLER[intid / 32] = 1u << (intid % 32);
}

uint32_t gic_ack(void)
{
    return GICC_IAR & 0x3ff;        // read = "I'm handling this one"
}

void gic_eoi(uint32_t intid)
{
    GICC_EOIR = intid;              // write = "done with this one"
}
```

- [ ] **Step 4: Write `kernel/timer.h`**

```c
#pragma once

#include <stdint.h>

// EL1 physical timer: private peripheral interrupt 14 -> INTID 16+14.
#define TIMER_INTID 30

void timer_init(uint32_t hz);
void timer_tick(void);
```

- [ ] **Step 5: Write `kernel/timer.c`**

```c
#include <stdint.h>
#include "timer.h"
#include "gic.h"
#include "kprintf.h"

// ARM generic timer, EL1 physical. No MMIO -- the timer lives in
// system registers. CNTFRQ_EL0 = counter frequency (fixed by the
// platform), CNTP_TVAL_EL0 = "fire this many ticks from now",
// CNTP_CTL_EL0 bit 0 = enable.

static uint64_t ticks_per_period;
static uint64_t tick_count;

static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline void write_cntp_tval(uint64_t v)
{
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(v));
}

static inline void write_cntp_ctl(uint64_t v)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(v));
}

void timer_init(uint32_t hz)
{
    uint64_t freq = read_cntfrq();

    ticks_per_period = freq / hz;
    kprintf("timer: counter runs at %d Hz, firing every %d ticks\n",
            (int)freq, (int)ticks_per_period);

    gic_enable_intid(TIMER_INTID);
    write_cntp_tval(ticks_per_period);  // first shot
    write_cntp_ctl(1);                  // enable, IRQ unmasked
}

void timer_tick(void)
{
    tick_count++;
    kprintf("tick %d\n", (int)tick_count);
    write_cntp_tval(ticks_per_period);  // re-arm: TVAL counts down anew
}
```

- [ ] **Step 6: Add `handle_irq` to `kernel/trap.c`**

Append after `handle_exception` (the struct and includes already exist; add `#include "gic.h"` and `#include "timer.h"` at the top):

```c
void handle_irq(struct trap_frame *tf);

void handle_irq(struct trap_frame *tf)
{
    (void)tf;   // milestone 6's scheduler will use the frame

    uint32_t intid = gic_ack();

    if (intid >= 1020)              // spurious: ack'd by someone else
        return;                     // (no EOI for spurious IDs)

    if (intid == TIMER_INTID)
        timer_tick();
    else
        panic("unexpected IRQ %d", (int)intid);

    gic_eoi(intid);
}
```

- [ ] **Step 7: Replace `kernel/main.c`**

```c
#include "kprintf.h"
#include "trap.h"
#include "gic.h"
#include "timer.h"

void kmain(void)
{
    exceptions_init();
    gic_init();
    timer_init(1);                          // 1 Hz heartbeat

    __asm__ volatile("msr daifclr, #2");    // unmask IRQs (the I in DAIF)
    kprintf("interrupts on; idling\n");

    for (;;)
        __asm__ volatile("wfi");            // sleep until an interrupt
}
```

- [ ] **Step 8: Update the Makefile**

`OBJS`:

```make
OBJS := build/boot.o build/main.o build/uart.o build/kprintf.o \
        build/vectors.o build/trap.o build/gic.o build/timer.o
```

C-rule header deps (gic.h and timer.h join):

```make
build/%.o: kernel/%.c kernel/uart.h kernel/kprintf.h kernel/trap.h \
           kernel/gic.h kernel/timer.h | build
	$(CC) $(CFLAGS) -c $< -o $@
```

- [ ] **Step 9: Build**

Run: `make`
Expected: no warnings; links `build/kernel.elf`.

- [ ] **Step 10: Scripted output check**

```bash
qemu-system-aarch64 -machine virt -cpu cortex-a72 -display none \
    -serial file:/tmp/os-serial.log -kernel build/kernel.elf & QPID=$!
sleep 5; kill $QPID
cat /tmp/os-serial.log
```

Expected (tick count depends on timing; at least 3):

```
timer: counter runs at 62500000 Hz, firing every 62500000 ticks
interrupts on; idling
tick 1
tick 2
tick 3
tick 4
```

Must-check: ticks keep arriving (proves the restore path returns cleanly and the re-arm works — a broken eret would stop after tick 1 or crash), and no exception dump appears.

- [ ] **Step 11: Commit**

```bash
git add kernel/vectors.S kernel/gic.h kernel/gic.c kernel/timer.h \
        kernel/timer.c kernel/trap.c kernel/main.c Makefile
git commit -m "milestone 4: GICv2, 1 Hz generic timer, IRQ restore path"
```

---

### Task 2: Milestone explainer doc

**Files:**
- Create: `docs/04-interrupts-and-timer.md`

**Interfaces:**
- Consumes: the code from Task 1.
- Produces: learning artifact in the established `docs/NN-topic.md` format.

- [ ] **Step 1: Write `docs/04-interrupts-and-timer.md`**

Cover exactly these sections, expanded into teaching prose with code from Task 1 inlined:

1. **Polling vs interrupts** — before this milestone the kernel could only *do* things; now hardware can tap it on the shoulder. `wfi` (wait-for-interrupt) + a timer = a kernel that sleeps at 0% CPU and wakes exactly when needed.
2. **The interrupt delivery chain** — device (timer) → GIC distributor (global routing: is it enabled? who gets it?) → GIC CPU interface (per-core: priority masking, ack/EOI) → CPU IRQ line → vector table entry 5. Every link must be enabled or nothing arrives: distributor CTLR, ISENABLER bit, PMR, CPU CTLR, and the DAIF I-bit — five switches, any one kills delivery (this is THE classic "why doesn't my interrupt fire" checklist).
3. **INTID space** — 0–15 SGIs (inter-core, milestone-future), 16–31 PPIs (per-core private peripherals — our timer is PPI 14 = INTID 30), 32+ SPIs (shared devices). 1020–1023 are special; 1023 = spurious, ack'd but nobody's; never EOI it.
4. **The ack/EOI protocol** — reading IAR claims the interrupt; writing EOIR releases it. Forget the EOI and the GIC politely never delivers that interrupt again. Forget to re-arm TVAL and the timer never fires again. Both bugs look identical from outside: "tick 1" then silence.
5. **The generic timer** — no MMIO; it lives in system registers (`CNTFRQ_EL0`, `CNTP_TVAL_EL0`, `CNTP_CTL_EL0`). TVAL is a countdown: fires when it hits zero, so periodic = re-arm in the handler. QEMU's counter runs at 62.5 MHz (read from CNTFRQ, printed at boot — real hardware differs, which is why we never hardcode it).
6. **eret: the road back** — walk the restore path: ELR/SPSR first (via scratch regs), GPRs in reverse, `eret` atomically restores PC from ELR and state from SPSR. The interrupted `wfi` loop never knows it was gone. Contrast with milestone 3's fatal path (`b .`). This save/restore discipline is 80% of a context switch — milestone 6 just swaps *which* frame gets restored.
7. **Try breaking it** — 3 experiments: comment out the `gic_eoi` call (tick 1, then silence); comment out the re-arm in `timer_tick` (same symptom, different cause — discuss how you'd tell them apart: check CNTP_CTL's ISTATUS bit); change `timer_init(1)` to `timer_init(10)` and watch the heartbeat accelerate.

- [ ] **Step 2: Commit**

```bash
git add docs/04-interrupts-and-timer.md
git commit -m "docs: milestone 4 explainer — GIC, generic timer, eret"
```

---

## Self-review notes

- Spec coverage: milestone 4 = "GIC init, ARM generic timer, periodic tick printed" — Task 1; explainer — Task 2. The restore path is required by "periodic" (a one-shot tick isn't periodic without surviving the first IRQ).
- GIC version and base addresses verified from QEMU's dumped DTB on this machine (not assumed).
- Type consistency: `gic_ack()` returns `uint32_t` consumed by `handle_irq`; `TIMER_INTID` defined once in `timer.h` and used in both `timer.c` and `trap.c`.
- Trap frame offsets in the restore path mirror milestone 3's save path exactly (248=ELR, 256=SPSR pair-loaded).
