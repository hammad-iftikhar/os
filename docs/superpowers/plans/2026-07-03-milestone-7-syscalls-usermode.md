# Milestone 7: Syscalls & User Mode — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A user program runs at EL0 in its own pages, talks to the kernel only through `svc` syscalls, coexists with a preempted kernel thread — and gets caught red-handed by the hardware when it tries to read kernel memory.

**Architecture:** The user program is a self-contained, position-independent assembly blob (`kernel/uprog.S`, collected into a `.user_code` section) that the kernel copies into a freshly allocated page and maps at VA `0x8000_0000` (L1 slot 2 — untouched by the identity map) with EL0-read+execute permissions; a second page at `0x8000_1000` becomes the EL0-writable user stack. `enter_user` (asm) programs `SP_EL0`/`ELR_EL1`/`SPSR_EL1` and `eret`s down to EL0. Vector entries 8 (sync from lower EL) and 9 (IRQ from lower EL) go live: 9 reuses `irq_stub` (the timer preempts user code exactly like kernel threads), 8 gets `sync_el0_stub` → `handle_sync_el0`, which dispatches `svc` (EC=0x15) to `kernel/syscall.c` and treats everything else as a fatal dump. `sys_write` validates the user pointer against the user VA range — the kernel's first trust boundary. Finale: after three greetings the user program dereferences `0x40080000` (kernel code) and the AP bits deliver a data abort from lower EL.

**Tech Stack:** Same toolchain. New: AP[7:6]/UXN/PXN page permission bits, L3 page descriptors, `SP_EL0`, SPSR crafting, `tlbi vmalle1`, `ic iallu` cache maintenance after code copy.

## Global Constraints

- Toolchain: `clang --target=aarch64-elf -ffreestanding -mgeneral-regs-only`; link with `ld.lld -T linker.ld` (no libc).
- Target: `qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 128M`.
- Error philosophy: panic loudly for kernel bugs. BUT user misbehavior must not be trusted: bad syscall numbers return `(uint64_t)-1` (a user bug is not a kernel bug); bad user pointers are rejected by validation; user faults produce the fatal dump (acceptable: we have exactly one user program and no notion of killing it — `ponytail:` ceiling, process teardown when there are processes to tear down).
- Commits: author Hammad Iftikhar <hammad.iftikhar723@gmail.com>; NO Co-Authored-By trailer.
- Build must stay warning-free under `-Wall -Wextra`.
- Syscall ABI (ours, Linux-flavored): number in `x8`, args in `x0..x5`, return in `x0`. `SYS_write=0` (x0 = user pointer to NUL-terminated string), `SYS_ticks=1` (returns timer tick count).
- User memory layout: code page VA `0x80000000` (EL0 RX, kernel no-exec), stack page VA `0x80001000` (EL0 RW, no-exec), stack top `0x80002000`.

## File Structure

```
kernel/
├── uprog.S      # NEW: the user program blob (.user_code section; NOT named
│                #      user.S -- build/%.o would collide with user.c)
├── user.h       # NEW: user_run()
├── user.c       # NEW: copy blob, map pages, enter_user
├── syscall.h    # NEW: syscall numbers + syscall_dispatch
├── syscall.c    # NEW: dispatch + sys_write pointer validation
├── trap.h       # MODIFIED: struct trap_frame moves here (syscall.c reads it)
├── trap.c       # MODIFIED: handle_sync_el0; frame struct no longer local
├── vectors.S    # MODIFIED: entries 8/9 live; shared restore_and_eret
├── swtch.S      # MODIFIED: enter_user added
├── vm.h         # MODIFIED: USER_* constants, vm_map_user
├── vm.c         # MODIFIED: remember L1 root; vm_map_user walks/builds L2->L3
├── main.c       # MODIFIED: one kernel worker + user_run
linker.ld        # MODIFIED: .user_code section with start/end symbols
Makefile         # MODIFIED: new objects, new headers in deps
```

---

### Task 1: User mode, syscalls, protection demo

One deliverable — the EL0 round trip. Mapping, entry, syscall, and fault paths only prove themselves together.

**Files:**
- Create: `kernel/uprog.S`, `kernel/user.h`, `kernel/user.c`, `kernel/syscall.h`, `kernel/syscall.c`
- Modify: `kernel/trap.h`, `kernel/trap.c`, `kernel/vectors.S`, `kernel/swtch.S`, `kernel/vm.h`, `kernel/vm.c`, `kernel/main.c`, `linker.ld`, `Makefile`
- Test: observable QEMU serial output (per spec)

**Interfaces:**
- Consumes: `alloc_page` (zeroed), `memcpy`, `timer_ticks()`, `kprintf`/`panic`, trap-frame layout, `schedule()`.
- Produces: `void user_run(void)` (never returns), `void vm_map_user(uint64_t va, uint64_t pa, int exec)`, `uint64_t syscall_dispatch(struct trap_frame *tf)`, asm `enter_user(uint64_t entry, uint64_t sp)`, linker symbols `__user_code_start/__user_code_end`, `struct trap_frame` now public in `trap.h`.

- [ ] **Step 1: Move `struct trap_frame` into `kernel/trap.h`**

Replace `kernel/trap.h` with:

```c
#pragma once

#include <stdint.h>

// Filled in by the vector stubs in vectors.S. Field order and offsets
// are a contract with that code -- do not reorder.
struct trap_frame {
    uint64_t x[31];     // x0..x30
    uint64_t elr;       // where the exception happened
    uint64_t spsr;      // saved processor state
    uint64_t esr;       // exception syndrome: what and why
    uint64_t far;       // faulting address (valid for aborts)
};

void exceptions_init(void);
```

And delete the same struct definition (and its comment) from `kernel/trap.c`.

- [ ] **Step 2: Add user constants and `vm_map_user` to `kernel/vm.h`**

```c
#pragma once

#include <stdint.h>

// User address space: L1 slot 2, far from the identity-mapped kernel
// (slot 0 = devices, slot 1 = RAM). One code page, one stack page.
#define USER_CODE_VA    0x80000000UL
#define USER_STACK_VA   0x80001000UL
#define USER_STACK_TOP  (USER_STACK_VA + 0x1000)

void vm_init(void);
void vm_map_user(uint64_t va, uint64_t pa, int exec);   // exec: RX else RW
```

- [ ] **Step 3: Extend `kernel/vm.c`**

Add after the existing `#define`s (keep everything already there):

```c
// L3 page descriptors and permission bits (unused at block levels above).
#define PTE_PAGE        (3UL)          // valid + page (L3 bits [1:0] = 0b11)
#define PTE_AP_USER_RW  (1UL << 6)     // AP=01: EL0 and EL1 read/write
#define PTE_AP_USER_RO  (3UL << 6)     // AP=11: EL0 and EL1 read-only
#define PTE_UXN         (1UL << 54)    // EL0 may not execute
#define PTE_PXN         (1UL << 53)    // EL1 may not execute
#define PA_MASK         0x0000fffffffff000UL

static uint64_t *kernel_l1;
```

In `vm_init`, after `uint64_t *l1 = alloc_page();` add:

```c
    kernel_l1 = l1;
```

Add at the end of the file:

```c
// Map one 4 KiB user page. Builds intermediate tables on demand.
// exec=1: user code (EL0 read+execute, kernel never executes it).
// exec=0: user data (EL0 read/write, nobody executes it).
void vm_map_user(uint64_t va, uint64_t pa, int exec)
{
    uint64_t *l2, *l3;
    int i1 = (va >> 30) & 0x1ff;
    int i2 = (va >> 21) & 0x1ff;
    int i3 = (va >> 12) & 0x1ff;

    if (!(kernel_l1[i1] & PTE_VALID)) {
        l2 = alloc_page();
        kernel_l1[i1] = (uint64_t)l2 | PTE_VALID | PTE_TABLE;
    } else {
        l2 = (uint64_t *)(kernel_l1[i1] & PA_MASK);
    }

    if (!(l2[i2] & PTE_VALID)) {
        l3 = alloc_page();
        l2[i2] = (uint64_t)l3 | PTE_VALID | PTE_TABLE;
    } else {
        l3 = (uint64_t *)(l2[i2] & PA_MASK);
    }

    uint64_t pte = pa | PTE_PAGE | PTE_ATTR(ATTRIDX_NORMAL) | PTE_AF |
                   PTE_SH_INNER | PTE_PXN;
    if (exec)
        pte |= PTE_AP_USER_RO;              // code: EL0 R+X (UXN clear)
    else
        pte |= PTE_AP_USER_RW | PTE_UXN;    // data: EL0 RW, no exec

    l3[i3] = pte;

    // New translation visible: flush stale TLB entries.
    __asm__ volatile("dsb ish\n tlbi vmalle1\n dsb ish\n isb");
}
```

- [ ] **Step 4: Write `kernel/uprog.S` — the user program**

```asm
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

    mov     x9, #0x400000       // crude delay so ticks pass between
2:  subs    x9, x9, #1          // greetings and K gets scheduled
    b.ne    2b

    subs    x20, x20, #1
    b.ne    1b

    adr     x0, lastmsg         // ...then announce the crime
    mov     x8, #0
    svc     #0

    mov     x0, #0x40000000     // kernel RAM base
    movk    x0, #0x0008, lsl #16    // 0x40080000: kernel .text
    ldr     x1, [x0]            // hardware says no: data abort, EL0

    // unreachable
3:  b       3b

umsg:    .asciz "[user] hello from EL0 via svc\n"
lastmsg: .asciz "[user] now reading kernel memory at 0x40080000...\n"
```

- [ ] **Step 5: Collect `.user_code` in `linker.ld`**

Change the `.rodata` line to:

```ld
    .rodata : {
        *(.rodata*)
        . = ALIGN(16);
        __user_code_start = .;
        *(.user_code)
        __user_code_end = .;
    }
```

- [ ] **Step 6: Add `enter_user` to `kernel/swtch.S`**

Append:

```asm
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
```

- [ ] **Step 7: Wire vector entries 8 and 9 in `kernel/vectors.S`**

Change the two table lines:

```asm
    ventry  8, sync_el0_stub        // sync from EL0: svc or user fault
    ventry  9, irq_stub             // timer preempts user code too
```

Then refactor the tail of the file so both survivable stubs share one
restore path — replace everything from `irq_stub:` to the final `eret`
with:

```asm
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
```

- [ ] **Step 8: Write `kernel/syscall.h`**

```c
#pragma once

#include <stdint.h>
#include "trap.h"

#define SYS_write   0
#define SYS_ticks   1

uint64_t syscall_dispatch(struct trap_frame *tf);
```

- [ ] **Step 9: Write `kernel/syscall.c`**

```c
#include <stdint.h>
#include "syscall.h"
#include "kprintf.h"
#include "timer.h"
#include "uart.h"
#include "vm.h"

// The trust boundary. Everything arriving from EL0 is hostile until
// proven otherwise -- a user pointer is just an integer someone at
// EL0 chose. Validate against the user's own address range before
// the kernel dereferences anything.

static uint64_t sys_write(uint64_t uptr)
{
    if (uptr < USER_CODE_VA || uptr >= USER_STACK_TOP) {
        kprintf("syscall: write with bad pointer %p, rejected\n",
                (void *)uptr);
        return (uint64_t)-1;
    }

    // Bounded walk: stop at NUL, the end of user memory, or 256 bytes.
    const char *s = (const char *)uptr;
    for (int i = 0; i < 256 && (uint64_t)&s[i] < USER_STACK_TOP; i++) {
        if (!s[i])
            return i;
        uart_putc(s[i] == '\n' ? '\r' : s[i]);
        if (s[i] == '\n')
            continue;
    }
    return (uint64_t)-1;
}

uint64_t syscall_dispatch(struct trap_frame *tf)
{
    switch (tf->x[8]) {
    case SYS_write:
        return sys_write(tf->x[0]);
    case SYS_ticks:
        return timer_ticks();
    default:
        // A user bug is not a kernel bug: refuse, don't panic.
        kprintf("syscall: unknown number %d\n", (int)tf->x[8]);
        return (uint64_t)-1;
    }
}
```

Note: `sys_write`'s newline handling has a subtle order — `uart_putc('\r')` then the `\n` itself must still print. Implement exactly as:

```c
        if (!s[i])
            return i;
        if (s[i] == '\n')
            uart_putc('\r');
        uart_putc(s[i]);
```

(the loop body above, corrected — use this version).

- [ ] **Step 10: Add `handle_sync_el0` to `kernel/trap.c`**

Add `#include "syscall.h"` at the top. Append after `handle_irq`:

```c
void handle_sync_el0(struct trap_frame *tf);

void handle_sync_el0(struct trap_frame *tf)
{
    uint32_t ec = (uint32_t)(tf->esr >> 26) & 0x3f;

    if (ec == 0x15) {                   // SVC: a syscall
        // ELR already points past the svc instruction; returning
        // resumes the user program with the result in its x0.
        tf->x[0] = syscall_dispatch(tf);
        return;
    }

    // Any other sync exception from EL0 is a user program fault.
    handle_exception(tf, 8);            // dumps registers and panics
}
```

`handle_exception` must be declared before use — its prototype already
sits above its definition; move that prototype (the
`void handle_exception(struct trap_frame *tf, uint64_t vector);` line)
above `handle_sync_el0` if needed (it is already file-scope; no change
required if `handle_sync_el0` is appended after `handle_exception`).

- [ ] **Step 11: Write `kernel/user.h` and `kernel/user.c`**

`kernel/user.h`:

```c
#pragma once

void user_run(void);    // copy, map, drop to EL0; never returns
```

`kernel/user.c`:

```c
#include <stdint.h>
#include "user.h"
#include "pmm.h"
#include "vm.h"
#include "string.h"
#include "kprintf.h"

extern char __user_code_start[], __user_code_end[];
extern void enter_user(uint64_t entry, uint64_t sp);

void user_run(void)
{
    void *code = alloc_page();
    void *stack = alloc_page();

    // The blob is position-independent; copy it out of the kernel
    // image into the user's own page.
    memcpy(code, __user_code_start,
           (unsigned long)(__user_code_end - __user_code_start));

    // We wrote instructions through the data side; make sure the
    // instruction side can't see stale bytes.
    __asm__ volatile("dc cvau, %0\n dsb ish\n ic iallu\n dsb ish\n isb"
                     :: "r"(code));

    vm_map_user(USER_CODE_VA, (uint64_t)code, 1);
    vm_map_user(USER_STACK_VA, (uint64_t)stack, 0);

    kprintf("user: %d bytes of code at VA %p, dropping to EL0\n",
            (int)(__user_code_end - __user_code_start),
            (void *)USER_CODE_VA);

    enter_user(USER_CODE_VA, USER_STACK_TOP);
}
```

- [ ] **Step 12: Replace `kernel/main.c`**

```c
#include "kprintf.h"
#include "trap.h"
#include "gic.h"
#include "timer.h"
#include "pmm.h"
#include "vm.h"
#include "thread.h"
#include "user.h"

// One kernel thread keeps running alongside the user program to show
// EL0 code and EL1 threads share the same scheduler heartbeat.
static void worker_k(void)
{
    for (int n = 1;; n++) {
        kprintf("  K (kernel thread) says %d\n", n);
        for (volatile int i = 0; i < 40000000; i++)
            ;
    }
}

void kmain(void)
{
    exceptions_init();
    gic_init();
    pmm_init();
    vm_init();
    thread_bootstrap();

    thread_create(worker_k, "K");

    timer_init(10);                         // 10 Hz preemption
    __asm__ volatile("msr daifclr, #2");

    // The main thread itself becomes the user program's vehicle:
    // every trap from EL0 lands on this thread's kernel stack.
    user_run();
}
```

- [ ] **Step 13: Update the Makefile**

`OBJS`:

```make
OBJS := build/boot.o build/main.o build/uart.o build/kprintf.o \
        build/vectors.o build/trap.o build/gic.o build/timer.o \
        build/string.o build/pmm.o build/vm.o build/swtch.o build/thread.o \
        build/syscall.o build/user.o build/uprog.o
```

Header deps gain `kernel/syscall.h kernel/user.h`:

```make
build/%.o: kernel/%.c kernel/uart.h kernel/kprintf.h kernel/trap.h \
           kernel/gic.h kernel/timer.h kernel/string.h kernel/pmm.h \
           kernel/vm.h kernel/thread.h kernel/syscall.h kernel/user.h | build
	$(CC) $(CFLAGS) -c $< -o $@
```

- [ ] **Step 14: Build and check output**

Run: `make` (no warnings), then:

```bash
qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 128M -display none \
    -serial file:/tmp/os-serial.log -kernel build/kernel.elf & QPID=$!
sleep 6; kill $QPID
cat /tmp/os-serial.log
```

Expected shape:

```
pmm: ... pages free ...
vm: MMU on, identity map, RAM=Normal MMIO=Device
thread: created 'K'
timer: counter runs at 62500000 Hz, firing every 6250000 ticks
user: 14X bytes of code at VA 0x0000000080000000, dropping to EL0
[user] hello from EL0 via svc
  K (kernel thread) says 1
[user] hello from EL0 via svc
[user] hello from EL0 via svc
  K (kernel thread) says 2
[user] now reading kernel memory at 0x40080000...

*** EXCEPTION: sync EL0/64 ***
ESR  = 0x0000000092000021  EC=24: data abort, lower EL
ELR  = 0x00000000800000XX  (faulting instruction)
FAR  = 0x0000000040080000  (faulting address)
...register dump...

PANIC: unhandled exception
```

Must-check, in order of importance:
1. `[user] hello from EL0 via svc` appears — the full EL0 round trip works (map → eret → svc → dispatch → validate → print → eret back).
2. K lines interleave with user lines — EL0 code and kernel threads share the scheduler; the timer preempts user mode via vector 9.
3. The final dump says `sync EL0/64` with `EC=24` (data abort, **lower** EL) and `FAR = 0x40080000` — hardware memory protection caught the user program, and `ELR` is a *user* address (0x80000xxx).

- [ ] **Step 15: Commit**

```bash
git add kernel/uprog.S kernel/user.h kernel/user.c kernel/syscall.h \
        kernel/syscall.c kernel/trap.h kernel/trap.c kernel/vectors.S \
        kernel/swtch.S kernel/vm.h kernel/vm.c kernel/main.c linker.ld Makefile
git commit -m "milestone 7: EL0 user mode, svc syscalls, memory protection"
```

---

### Task 2: Milestone explainer doc

**Files:**
- Create: `docs/07-syscalls-and-user-mode.md`

**Interfaces:**
- Consumes: code from Task 1.
- Produces: learning artifact; final doc of the original roadmap.

- [ ] **Step 1: Write `docs/07-syscalls-and-user-mode.md`**

Cover exactly these sections, expanded into teaching prose with code inlined:

1. **What "user mode" buys** — EL0 is the hardware's promise: code there cannot touch kernel memory, MMIO, or system registers, no matter what it executes. The kernel stops being "the whole program" and becomes a *service* with a hardware-enforced API. This is the line between a program that multitasks and an operating system.
2. **Building the user's world** — pages from `alloc_page`, the position-independent blob copied out of the kernel image (no filesystem yet — `ponytail:` ceiling: an ELF loader when there's a disk to load from), the AP/UXN/PXN permission matrix as a table (code: EL0 RX + PXN; stack: EL0 RW + UXN + PXN; kernel RAM: EL0 nothing). Why PXN on user code matters (kernel tricked into jumping to user-controlled instructions = privilege escalation).
3. **The cache-maintenance wart** — we wrote *instructions* through the *data* side; `dc cvau` + `ic iallu` + `isb` make the instruction fetcher see them. Works in QEMU without it; real silicon eventually executes stale garbage — same "TCG isn't hardware" lesson as milestone 5's `isb`.
4. **eret down, svc up** — `enter_user` walkthrough: `SP_EL0` (EL0 has its own stack register — traps land back on the kernel stack automatically), `SPSR = 0` = "EL0t, interrupts unmasked", `eret` as the one-way door. `svc` as its mirror: ELR points *past* the svc; the trap frame's x8 carries the number; writing `tf->x[0]` delivers the return value into the user's world. Round trip in ~10 lines each way.
5. **The trust boundary** — `sys_write` validation dissected: a user pointer is an integer chosen at EL0; the bounded walk (NUL, range end, 256 bytes); why "user bug ≠ kernel bug" (unknown syscall returns −1 instead of panicking — the one place our fail-loud philosophy yields to a different principle: never let the untrusted side choose when you die).
6. **One scheduler, two worlds** — vector 9 → the same `irq_stub`: the timer preempts EL0 exactly as it preempts kernel threads (the eret at the end restores SPSR, which remembers the interrupted world was EL0). K's lines interleaving with user greetings is the proof. The main thread as the user program's "vehicle": every trap from EL0 lands on its kernel stack.
7. **The finale, decoded** — the dump analyzed line by line: `sync EL0/64` (vector 8, not 4 — the *lower EL* row finally used), `EC=24` vs milestone 3's `EC=25` (same abort, different origin EL — one bit in ESR distinguishes "kernel bug" from "user caught"), `ELR = 0x80000xxx` (a user address: the crime scene is in user space), `FAR = 0x40080000` (the kernel address it reached for). The AP bits did this — no kernel code checked anything.
8. **Try breaking it** — 3 experiments: make the user program pass a kernel address to `SYS_write` (validation rejects it; the kernel *didn't* dereference — compare with the crash you'd get if you deleted the check); call syscall 99 (polite −1); delete `PTE_UXN` from the stack mapping, put instructions on the stack and jump there (executable-stack attack, live — then put UXN back and watch it become an instruction abort).
9. **Where the roadmap ends, and what's beyond** — the seven milestones recapped in one paragraph each →  the kernel now boots, talks, catches faults, keeps time, manages memory, schedules threads, and runs protected user programs. Beyond the original spec: filesystem + ELF loading, multiple user processes (per-process TTBR0 switching), higher-half kernel (deferred 5b), SMP (the parked cores from milestone 1 wake up), real hardware (Raspberry Pi port).

- [ ] **Step 2: Commit**

```bash
git add docs/07-syscalls-and-user-mode.md
git commit -m "docs: milestone 7 explainer — EL0, syscalls, trust boundary"
```

---

## Self-review notes

- Spec coverage: milestone 7 = "drop to EL0, `svc`-based syscalls, a first 'user program'" — Task 1 delivers all three plus the protection demo; Task 2 closes the docs series with the roadmap retrospective the spec's learning goal implies.
- Filename collision (`user.S` vs `user.c` → same `build/user.o`) avoided by naming the blob `uprog.S`.
- Type consistency: `struct trap_frame` moves to `trap.h` before `syscall.h` includes it; `handle_sync_el0` name matches vectors.S ↔ trap.c; `enter_user(uint64_t, uint64_t)` matches user.c's extern; `USER_*` constants defined once in vm.h, consumed by user.c and syscall.c.
- Step 9 contains a corrected loop body — the final code in the "use this version" block is normative.
- The `x[8]` syscall-number slot and `x[0]` return slot match the trap-frame x-array layout from milestone 3 (x0 at offset 0, x8 at offset 64).
