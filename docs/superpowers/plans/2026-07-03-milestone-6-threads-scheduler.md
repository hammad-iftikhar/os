# Milestone 6: Threads & Scheduler — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preemptive multitasking — kernel threads with their own stacks, an xv6-style `swtch` context switch (callee-saved registers + sp), and round-robin scheduling driven by the timer tick.

**Architecture:** `kernel/swtch.S` saves x19–x30 + sp into the outgoing thread's `struct context` and loads the incoming one's (caller-saved registers don't need saving — `swtch` is a function call, the ABI already made the caller spill them; an interrupted thread's full state lives in its trap frame *on its own stack*). A fixed table of 8 threads in `kernel/thread.c`; `kmain` becomes thread 0. On each timer IRQ, `handle_irq` re-arms, **EOIs, and only then** calls `schedule()` — EOI-after-switch would leave the timer interrupt active in the GIC and kill all future preemption (the milestone's signature bug, documented in the doc task). New threads start via a `thread_start` trampoline that unmasks IRQs and jumps to the thread function. `kprintf` becomes atomic (masks IRQs while printing) so preempted threads don't interleave characters.

**Tech Stack:** Same toolchain. New concepts: AArch64 procedure-call standard (AAPCS64) callee-saved registers, per-thread kernel stacks from `alloc_page`.

## Global Constraints

- Toolchain: `clang --target=aarch64-elf -ffreestanding -mgeneral-regs-only`; link with `ld.lld -T linker.ld` (no libc).
- Target: `qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 128M`.
- Error philosophy: panic loudly (thread-function return panics; no free slot panics).
- Commits: author Hammad Iftikhar <hammad.iftikhar723@gmail.com>; NO Co-Authored-By trailer.
- Build must stay warning-free under `-Wall -Wextra`.
- Deferred by design: thread exit/join (workers run forever), sleep/block states, priorities, SMP. Fixed table of 8 threads (`ponytail:` comment marks the ceiling).

## File Structure

```
kernel/
├── swtch.S      # NEW: swtch(old, new) + thread_start trampoline
├── thread.h     # NEW: struct context (asm contract), thread_create, schedule, thread_bootstrap
├── thread.c     # NEW: thread table, round-robin scheduler
├── kprintf.c    # MODIFIED: mask IRQs during kprintf/panic (atomic prints)
├── trap.c       # MODIFIED: handle_irq reordered — re-arm, EOI, THEN schedule
├── timer.c      # MODIFIED: timer_tick stops printing (threads are the demo now)
├── main.c       # MODIFIED: become thread 0, spawn two workers
Makefile         # MODIFIED: swtch.o, thread.o, thread.h dep
```

**struct context layout contract** (`swtch.S` writes/reads these exact offsets):

| Offset | Field |
|---|---|
| 0–72 | x19–x28 (callee-saved) |
| 80 | x29 (frame pointer) |
| 88 | x30 (link register — where `swtch` returns to) |
| 96 | sp |

---

### Task 1: swtch, thread table, preemptive round-robin

**Files:**
- Create: `kernel/swtch.S`, `kernel/thread.h`, `kernel/thread.c`
- Modify: `kernel/kprintf.c`, `kernel/trap.c`, `kernel/timer.c`, `kernel/main.c`, `Makefile`
- Test: observable QEMU serial output (per spec)

**Interfaces:**
- Consumes: `alloc_page()` (milestone 5), `TIMER_INTID`/`timer_tick` (milestone 4), `gic_eoi` (milestone 4), `kprintf`/`panic`.
- Produces: `void thread_bootstrap(void)`, `void thread_create(void (*fn)(void), const char *name)`, `void schedule(void)` (called by `handle_irq`; milestone 7's syscall layer will also call it); asm `swtch(struct context *old, struct context *new)`; `uint64_t timer_ticks(void)`.

- [ ] **Step 1: Write `kernel/swtch.S`**

```asm
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
```

- [ ] **Step 2: Write `kernel/thread.h`**

```c
#pragma once

#include <stdint.h>

// Callee-saved state, the only registers that survive a function call.
// Field offsets are a contract with swtch.S -- do not reorder.
struct context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t x29;   // frame pointer
    uint64_t x30;   // link register: where swtch returns to
    uint64_t sp;
};

enum thread_state {
    THREAD_UNUSED = 0,
    THREAD_RUNNABLE,
    THREAD_RUNNING,
};

struct thread {
    struct context ctx;
    enum thread_state state;
    const char *name;
};

void thread_bootstrap(void);    // adopt kmain as thread 0
void thread_create(void (*fn)(void), const char *name);
void schedule(void);            // round-robin; call with IRQs masked
```

- [ ] **Step 3: Write `kernel/thread.c`**

```c
#include <stdint.h>
#include "thread.h"
#include "pmm.h"
#include "kprintf.h"

// ponytail: fixed table of 8 threads, no exit/join -- workers run
// forever. Dynamic thread lifecycle when a milestone needs it.
#define NTHREAD 8

extern void swtch(struct context *old, struct context *new);
extern void thread_start(void);

static struct thread threads[NTHREAD];
static int current;

void thread_bootstrap(void)
{
    // kmain is already running on the boot stack; give it a slot so
    // it can be switched away from like everyone else.
    threads[0].state = THREAD_RUNNING;
    threads[0].name = "main";
    current = 0;
}

void thread_create(void (*fn)(void), const char *name)
{
    struct thread *t = 0;

    for (int i = 0; i < NTHREAD; i++) {
        if (threads[i].state == THREAD_UNUSED) {
            t = &threads[i];
            break;
        }
    }
    if (!t)
        panic("thread_create: no free slot for %s", name);

    void *stack = alloc_page();     // 4 KiB kernel stack

    t->name = name;
    t->ctx.x19 = (uint64_t)fn;      // thread_start jumps via x19
    t->ctx.x30 = (uint64_t)thread_start;
    t->ctx.sp = (uint64_t)stack + PAGE_SIZE;    // stacks grow down
    t->state = THREAD_RUNNABLE;

    kprintf("thread: created '%s'\n", name);
}

// Round-robin: next runnable slot after the current one, wrapping.
// Called with IRQs masked (from the timer IRQ path).
void schedule(void)
{
    int next = -1;

    for (int i = 1; i <= NTHREAD; i++) {
        int cand = (current + i) % NTHREAD;
        if (threads[cand].state == THREAD_RUNNABLE) {
            next = cand;
            break;
        }
    }
    if (next < 0)
        return;                     // nobody else wants the CPU

    struct thread *old = &threads[current];
    struct thread *new = &threads[next];

    old->state = THREAD_RUNNABLE;
    new->state = THREAD_RUNNING;
    current = next;
    swtch(&old->ctx, &new->ctx);
    // When someone switches back to us, we resume right here.
}

void thread_return_panic(void);

void thread_return_panic(void)
{
    panic("thread '%s' returned", threads[current].name);
}
```

- [ ] **Step 4: Make kprintf atomic in `kernel/kprintf.c`**

Preemption means a thread can be switched out mid-print and another
thread prints into the middle of its line. Mask IRQs for the duration
of each kprintf. Replace `kprintf` and `panic` with:

```c
void kprintf(const char *fmt, ...)
{
    uint64_t daif;
    va_list ap;

    // Atomic prints: no preemption mid-line. Save/restore rather than
    // set/clear so printing from IRQ context stays masked.
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2");

    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);

    __asm__ volatile("msr daif, %0" :: "r"(daif));
}

_Noreturn void panic(const char *fmt, ...)
{
    va_list ap;

    __asm__ volatile("msr daifset, #2");    // no preemption while dying

    kprintf("\nPANIC: ");
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");

    for (;;)
        __asm__ volatile("wfe");
}
```

(`#include <stdint.h>` is already present in kprintf.c.)

- [ ] **Step 5: Reorder `handle_irq` in `kernel/trap.c`**

Add `#include "thread.h"` at the top. Replace `handle_irq` with:

```c
void handle_irq(struct trap_frame *tf)
{
    (void)tf;

    uint32_t intid = gic_ack();

    if (intid >= 1020)              // spurious: ack'd by someone else
        return;                     // (no EOI for spurious IDs)

    if (intid != TIMER_INTID)
        panic("unexpected IRQ %d", (int)intid);

    // Order is load-bearing: re-arm and EOI *before* schedule().
    // schedule() may not return for many ticks (we'll be switched
    // out); if the EOI waited until after, the GIC would consider
    // the timer interrupt still active and never deliver another
    // one -- preemption would end at the first switch.
    timer_tick();
    gic_eoi(intid);
    schedule();
}
```

- [ ] **Step 6: Quiet the timer in `kernel/timer.c`**

The heartbeat print served milestone 4; threads are the visible demo
now. Replace `timer_tick` and add a tick counter accessor:

```c
void timer_tick(void)
{
    tick_count++;
    write_cntp_tval(ticks_per_period);  // re-arm: TVAL counts down anew
}

uint64_t timer_ticks(void)
{
    return tick_count;
}
```

And in `kernel/timer.h`, add the accessor declaration:

```c
uint64_t timer_ticks(void);
```

- [ ] **Step 7: Replace `kernel/main.c`**

```c
#include "kprintf.h"
#include "trap.h"
#include "gic.h"
#include "timer.h"
#include "pmm.h"
#include "vm.h"
#include "thread.h"

// Two workers that never yield voluntarily: only preemption can
// interleave them. The busy loop is the point -- it proves the timer
// can yank the CPU away from uncooperative code.
static void spin(void)
{
    for (volatile int i = 0; i < 20000000; i++)
        ;
}

static void worker_a(void)
{
    for (int n = 1;; n++) {
        kprintf("  A says %d\n", n);
        spin();
    }
}

static void worker_b(void)
{
    for (int n = 1;; n++) {
        kprintf("  B says %d\n", n);
        spin();
    }
}

void kmain(void)
{
    exceptions_init();
    gic_init();
    pmm_init();
    vm_init();
    thread_bootstrap();

    thread_create(worker_a, "A");
    thread_create(worker_b, "B");

    timer_init(10);                         // 10 Hz preemption
    __asm__ volatile("msr daifclr, #2");
    kprintf("scheduler on; main thread idling\n");

    for (;;)
        __asm__ volatile("wfi");
}
```

- [ ] **Step 8: Update the Makefile**

`OBJS`:

```make
OBJS := build/boot.o build/main.o build/uart.o build/kprintf.o \
        build/vectors.o build/trap.o build/gic.o build/timer.o \
        build/string.o build/pmm.o build/vm.o build/swtch.o build/thread.o
```

Header deps gain `kernel/thread.h`:

```make
build/%.o: kernel/%.c kernel/uart.h kernel/kprintf.h kernel/trap.h \
           kernel/gic.h kernel/timer.h kernel/string.h kernel/pmm.h \
           kernel/vm.h kernel/thread.h | build
	$(CC) $(CFLAGS) -c $< -o $@
```

(`kernel/swtch.S` is covered by the existing `build/%.o: kernel/%.S` rule.)

- [ ] **Step 9: Build and check output**

Run: `make` (no warnings), then:

```bash
qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 128M -display none \
    -serial file:/tmp/os-serial.log -kernel build/kernel.elf & QPID=$!
sleep 6; kill $QPID
cat /tmp/os-serial.log
```

Expected shape (counts vary with host speed):

```
pmm: 32634 pages free ...
vm: MMU on, identity map, RAM=Normal MMIO=Device
thread: created 'A'
thread: created 'B'
timer: counter runs at 62500000 Hz, firing every 6250000 ticks
scheduler on; main thread idling
  A says 1
  A says 2
  B says 1
  B says 2
  A says 3
  ...
```

Must-check: **both** A and B lines keep appearing, interleaved in runs
(each runs until a tick preempts it), counts increase monotonically per
thread, no line is garbled mid-characters (kprintf atomicity), and no
exception dump appears. A and B never voluntarily yield — every switch
you see is the timer forcibly taking the CPU.

- [ ] **Step 10: Commit**

```bash
git add kernel/swtch.S kernel/thread.h kernel/thread.c kernel/kprintf.c \
        kernel/trap.c kernel/timer.h kernel/timer.c kernel/main.c Makefile
git commit -m "milestone 6: kernel threads, swtch, preemptive round-robin"
```

---

### Task 2: Milestone explainer doc

**Files:**
- Create: `docs/06-threads-and-scheduling.md`

**Interfaces:**
- Consumes: code from Task 1.
- Produces: learning artifact in the established `docs/NN-topic.md` format.

- [ ] **Step 1: Write `docs/06-threads-and-scheduling.md`**

Cover exactly these sections, expanded into teaching prose with code inlined:

1. **What a thread actually is** — a stack plus saved registers; nothing more. The thread table entry is ~120 bytes; the stack is one page. "Multitasking" = the CPU's registers are a shared resource that threads take turns wearing.
2. **Why swtch saves only x19–x30 + sp** — the AAPCS64 insight: `swtch` is an ordinary function call, so the caller already spilled x0–x18 (caller-saved) per the ABI. Callee-saved registers are *defined* as "state that survives a function call" — so they're exactly the state that survives a context switch. The full-register case (preemption) is already handled: the trap frame on the interrupted thread's own stack holds everything; swtch switches *which stack* — and therefore which trap frame — eventually unwinds and erets.
3. **Anatomy of a preemptive switch** — trace tick-by-tick: thread A busy-spinning → timer IRQ → `irq_stub` saves A's full frame on A's stack → `handle_irq` → `schedule()` → `swtch(&A.ctx, &B.ctx)` — A's callee-saved regs parked in its context, sp now B's stack → return lands in B's `schedule()` call frame (or `thread_start` for a fresh B) → unwind → B's `eret` from *its* last interruption → B continues mid-`spin()`. The diagram of two stacks with trap frames.
4. **Birth of a thread** — `thread_create` forges a context by hand: x30 = `thread_start`, x19 = the function, sp = fresh page top. The trampoline's two jobs: unmask IRQs (we arrive on the masked IRQ path) and `blr x19`. A forged return address makes `swtch`'s `ret` "return" somewhere no call ever came from — the same trick, pointed the other way, is how attackers do ROP; here it's how threads are born.
5. **The EOI-before-schedule bug** — the milestone's signature trap, in full: if `gic_eoi` came after `schedule()`, the interrupt stays *active* in the GIC across the switch; thread B runs but the GIC will never signal INTID 30 again; preemption dies at the first switch, B keeps the CPU forever, and the symptom ("B says 1, 2, 3... forever, A never speaks again") looks exactly like a scheduler bug even though the scheduler is perfect. Ordering in interrupt handlers is architecture.
6. **Atomic kprintf** — preemption's first casualty is shared state; the UART is shared state. A thread preempted mid-line interleaves characters with the next thread's print. Fix: mask IRQs across the print, saving/restoring DAIF (not blind set/clear — printing from inside the IRQ handler must stay masked). This is the project's first *critical section*, and the pattern (save-mask-restore) is the seed of every lock the kernel will ever have.
7. **What the demo proves** — A and B never yield, never sleep, never cooperate; the busy `spin()` is deliberate. Every interleave in the output is the timer forcibly confiscating the CPU. Runs of "A says n / A says n+1" show the 10 Hz time slice vs. print+spin duration; count the lines per run and estimate the spin time.
8. **Try breaking it** — 3 experiments: swap the EOI/schedule order and watch B monopolize forever (the signature bug, live); remove the DAIF save/restore from kprintf and hunt for garbled lines; create a third worker C and predict the rotation order before running (round-robin is deterministic: A→B→C→main→A...).

- [ ] **Step 2: Commit**

```bash
git add docs/06-threads-and-scheduling.md
git commit -m "docs: milestone 6 explainer — swtch, preemption, EOI ordering"
```

---

## Self-review notes

- Spec coverage: milestone 6 = "kernel threads, context switch (x19–x30, sp), round-robin on the timer tick" — Task 1 implements exactly that contract; Task 2 the doc.
- Type consistency: `struct context` offsets (0–96) match `swtch.S` stp/ldp offsets; `thread_start`/`thread_return_panic` names match between swtch.S and thread.c; `schedule()` signature matches trap.c's call.
- The timer print removed in Step 6 was milestone 4's demo output; its own doc still describes it accurately *for that milestone's code* (git history preserves it).
- kprintf atomicity folded into this milestone because preemption is what makes it necessary (not speculative).
