# Milestone 5: Memory — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The kernel manages its own RAM — a free-list physical page allocator, then page tables and the MMU switched on with RAM as cacheable Normal memory and MMIO as Device memory.

**Architecture:** `kernel/pmm.c` threads a free list *through* the free pages themselves (zero metadata overhead), managing RAM from `_kernel_end` (new linker symbol) to the 128 MiB boundary. `kernel/vm.c` builds a 39-bit-VA, 4 KiB-granule identity map: L1 entry 0 = one 1 GiB Device block (flash, GIC, UART), L1 entry 1 = an L2 table of 64 × 2 MiB Normal blocks (exactly our 128 MiB of RAM), then programs `MAIR/TCR/TTBR0` and flips `SCTLR_EL1.{M,C,I}`. Kernel stays identity-mapped — **higher-half is deliberately deferred** (nothing in milestones 6–7 requires it; it can become milestone 5b on request). Demo payoff: milestone 3's unaligned read stops crashing because RAM is now Normal memory, and the timer keeps ticking to prove interrupts survive the MMU.

**Tech Stack:** Same toolchain. New: AArch64 translation regime (`MAIR_EL1`, `TCR_EL1`, `TTBR0_EL1`, `SCTLR_EL1`), 4 KiB translation granule. QEMU gets an explicit `-m 128M` so the hardcoded RAM size can't silently drift.

## Global Constraints

- Toolchain: `clang --target=aarch64-elf -ffreestanding -mgeneral-regs-only`; link with `ld.lld -T linker.ld` (no libc).
- Target: `qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 128M` (the `-m 128M` is new and mandatory from this milestone).
- Error philosophy: panic loudly (out-of-memory panics; misaligned frees panic).
- Commits: author Hammad Iftikhar <hammad.iftikhar723@gmail.com>; NO Co-Authored-By trailer.
- Build must stay warning-free under `-Wall -Wextra`.
- RAM: base `0x4000_0000`, size 128 MiB → end `0x4800_0000`. Page size 4096 bytes.
- Deferred by design: higher-half kernel, `free`-side coalescing/guard against double-free, mapping RAM with 4 KiB granularity (2 MiB blocks suffice until user pages need fine permissions in milestone 7).

## File Structure

```
kernel/
├── string.h     # NEW: memset, memcpy (freestanding must supply its own)
├── string.c     # NEW
├── pmm.h        # NEW: pmm_init, alloc_page, free_page, pmm_free_count
├── pmm.c        # NEW: free-list physical allocator
├── vm.h         # NEW: vm_init
├── vm.c         # NEW: page-table construction + MMU enable
├── main.c       # MODIFIED: init order + demos (allocator, unaligned read)
linker.ld        # MODIFIED: page-align end, export _kernel_end
Makefile         # MODIFIED: new objects, -m 128M, header deps
```

---

### Task 1: Physical page allocator

**Files:**
- Create: `kernel/string.h`, `kernel/string.c`, `kernel/pmm.h`, `kernel/pmm.c`
- Modify: `linker.ld` (add `_kernel_end`), `kernel/main.c`, `Makefile`
- Test: observable QEMU serial output (per spec)

**Interfaces:**
- Consumes: `kprintf`/`panic` (milestone 2); linker-symbol pattern from milestone 1 (`__bss_start` et al.).
- Produces: `void *alloc_page(void)` (4 KiB, zeroed), `void free_page(void *p)`, `void pmm_init(void)`, `uint64_t pmm_free_count(void)`; `void *memset(void *dst, int c, unsigned long n)`, `void *memcpy(void *dst, const void *src, unsigned long n)`; linker symbol `_kernel_end`. Task 2's page-table code allocates tables with `alloc_page`.

- [ ] **Step 1: Write `kernel/string.h`**

```c
#pragma once

void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);
```

- [ ] **Step 2: Write `kernel/string.c`**

```c
#include "string.h"

// Freestanding code must supply these. The compiler also assumes they
// exist -- clang may lower struct zeroing or copies into calls to
// memset/memcpy even at -ffreestanding.

void *memset(void *dst, int c, unsigned long n)
{
    unsigned char *d = dst;

    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}
```

- [ ] **Step 3: Export `_kernel_end` from `linker.ld`**

After the `__stack_top = .;` line, add:

```ld
    . = ALIGN(4096);
    _kernel_end = .;            /* first byte the allocator may hand out */
```

- [ ] **Step 4: Write `kernel/pmm.h`**

```c
#pragma once

#include <stdint.h>

#define PAGE_SIZE 4096UL

void pmm_init(void);
void *alloc_page(void);         // 4 KiB, zeroed; panics when out
void free_page(void *p);
uint64_t pmm_free_count(void);
```

- [ ] **Step 5: Write `kernel/pmm.c`**

```c
#include <stdint.h>
#include "pmm.h"
#include "string.h"
#include "kprintf.h"

// Free-list allocator. The trick: free pages store the list themselves.
// The first 8 bytes of each free page hold a pointer to the next free
// page -- so tracking N free pages costs zero bytes of metadata.
//
// Managed range: _kernel_end (everything below is kernel image + boot
// stack) up to the end of RAM. RAM size matches the Makefile's -m 128M.

#define RAM_END 0x48000000UL    // 0x40000000 + 128 MiB

struct free_page {
    struct free_page *next;
};

static struct free_page *free_list;
static uint64_t free_count;

void pmm_init(void)
{
    extern char _kernel_end[];
    uint64_t first = (uint64_t)_kernel_end;

    for (uint64_t pa = first; pa + PAGE_SIZE <= RAM_END; pa += PAGE_SIZE)
        free_page((void *)pa);

    kprintf("pmm: %d pages free (%d KiB), first at %p\n",
            (int)free_count, (int)(free_count * PAGE_SIZE / 1024),
            (void *)first);
}

void *alloc_page(void)
{
    struct free_page *p = free_list;

    if (!p)
        panic("pmm: out of memory");
    free_list = p->next;
    free_count--;

    memset(p, 0, PAGE_SIZE);    // callers rely on zeroed pages
    return p;
}

void free_page(void *p)
{
    if ((uint64_t)p % PAGE_SIZE)
        panic("pmm: freeing misaligned pointer %p", p);

    struct free_page *fp = p;

    fp->next = free_list;
    free_list = fp;
    free_count++;
}
```

- [ ] **Step 6: Add the allocator demo to `kernel/main.c`**

Replace the whole file:

```c
#include "kprintf.h"
#include "trap.h"
#include "gic.h"
#include "timer.h"
#include "pmm.h"

void kmain(void)
{
    exceptions_init();
    gic_init();
    pmm_init();

    // Allocator smoke test: LIFO free list means freeing then
    // reallocating returns the same page.
    void *a = alloc_page();
    void *b = alloc_page();
    kprintf("pmm: a=%p b=%p\n", a, b);
    free_page(a);
    void *c = alloc_page();
    kprintf("pmm: freed a, realloc'd -> %p (%s)\n",
            c, c == a ? "same page, LIFO works" : "BUG: expected a");

    timer_init(1);
    __asm__ volatile("msr daifclr, #2");
    kprintf("interrupts on; idling\n");

    for (;;)
        __asm__ volatile("wfi");
}
```

- [ ] **Step 7: Update the Makefile**

QEMU line gains explicit memory size (must match pmm.c's `RAM_END`):

```make
QEMU   := qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 128M
```

`OBJS`:

```make
OBJS := build/boot.o build/main.o build/uart.o build/kprintf.o \
        build/vectors.o build/trap.o build/gic.o build/timer.o \
        build/string.o build/pmm.o
```

Header deps:

```make
build/%.o: kernel/%.c kernel/uart.h kernel/kprintf.h kernel/trap.h \
           kernel/gic.h kernel/timer.h kernel/string.h kernel/pmm.h | build
	$(CC) $(CFLAGS) -c $< -o $@
```

- [ ] **Step 8: Build and check output**

Run: `make` (expect: no warnings), then:

```bash
qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 128M -display none \
    -serial file:/tmp/os-serial.log -kernel build/kernel.elf & QPID=$!
sleep 3; kill $QPID
cat /tmp/os-serial.log
```

Expected (addresses approximate; `a`/`c` identical is the must-check, and
free count ≈ 32700 (128 MiB minus kernel image + stack):

```
pmm: 32XXX pages free (13XXXX KiB), first at 0x00000000400aX000
pmm: a=0x00000000400aX000 b=0x00000000400aY000
pmm: freed a, realloc'd -> 0x00000000400aX000 (same page, LIFO works)
interrupts on; idling
tick 1
tick 2
```

- [ ] **Step 9: Commit**

```bash
git add kernel/string.h kernel/string.c kernel/pmm.h kernel/pmm.c \
        kernel/main.c linker.ld Makefile
git commit -m "milestone 5a: free-list physical page allocator"
```

---

### Task 2: Page tables and MMU on

**Files:**
- Create: `kernel/vm.h`, `kernel/vm.c`
- Modify: `kernel/main.c` (call `vm_init`, add the unaligned-read payoff demo), `Makefile`
- Test: observable QEMU serial output (per spec)

**Interfaces:**
- Consumes: `alloc_page()` (Task 1, returns zeroed 4 KiB), `kprintf`.
- Produces: `void vm_init(void)`. Milestone 7 will extend `vm.c` with user mappings; the descriptor-bit macros defined here (`PTE_VALID`, `PTE_TABLE`, `PTE_BLOCK`, `PTE_AF`, `PTE_DEVICE`, `PTE_NORMAL`) are its vocabulary.

- [ ] **Step 1: Write `kernel/vm.h`**

```c
#pragma once

void vm_init(void);
```

- [ ] **Step 2: Write `kernel/vm.c`**

```c
#include <stdint.h>
#include "vm.h"
#include "pmm.h"
#include "kprintf.h"

// Identity-mapped virtual memory: VA == PA, but with real memory
// attributes at last -- RAM becomes cacheable Normal memory, MMIO
// stays strictly-ordered Device memory.
//
// Layout: 4 KiB granule, 39-bit VA (T0SZ=25) = 3-level walk.
//   L1: 512 entries x 1 GiB   L2: 512 x 2 MiB   L3: 512 x 4 KiB
//
//   L1[0] = 1 GiB Device block  (0x00000000: flash, GIC, UART, ...)
//   L1[1] = table -> L2, 64 x 2 MiB Normal blocks (our 128 MiB RAM)
//
// No L3 needed yet: 2 MiB blocks are fine until user pages need
// per-4KiB permissions (milestone 7).

#define MAIR_DEVICE_nGnRnE  0x00
#define MAIR_NORMAL_WB      0xff
// index into MAIR_EL1, set below: attr0 = device, attr1 = normal
#define ATTRIDX_DEVICE      0
#define ATTRIDX_NORMAL      1

#define PTE_VALID       (1UL << 0)
#define PTE_TABLE       (1UL << 1)     // level 0-2: next-level table
#define PTE_BLOCK       (0UL << 1)     // level 1-2: big leaf block
#define PTE_AF          (1UL << 10)    // access flag: MUST be set, or
                                       // first touch = translation fault
#define PTE_SH_INNER    (3UL << 8)     // inner shareable (SMP-safe)
#define PTE_ATTR(i)     ((uint64_t)(i) << 2)

#define PTE_DEVICE      (PTE_VALID | PTE_BLOCK | PTE_ATTR(ATTRIDX_DEVICE) | PTE_AF)
#define PTE_NORMAL      (PTE_VALID | PTE_BLOCK | PTE_ATTR(ATTRIDX_NORMAL) | PTE_AF | PTE_SH_INNER)

#define RAM_BASE        0x40000000UL
#define RAM_SIZE        (128UL << 20)
#define BLOCK_2M        (2UL << 20)

// TCR_EL1: T0SZ=25 (39-bit VA), 4 KiB granule (TG0=0), write-back
// cacheable walks (IRGN0/ORGN0=1), inner shareable (SH0=3),
// EPD1=1 (TTBR1 unused -- walks through it disabled), IPS=40-bit PA.
#define TCR_VALUE ( \
    25UL            /* T0SZ  */ | \
    (1UL << 8)      /* IRGN0 */ | \
    (1UL << 10)     /* ORGN0 */ | \
    (3UL << 12)     /* SH0   */ | \
    (1UL << 23)     /* EPD1  */ | \
    (2UL << 32)     /* IPS   */ )

void vm_init(void)
{
    uint64_t *l1 = alloc_page();    // zeroed: all entries invalid
    uint64_t *l2 = alloc_page();

    // First GiB: devices, identity, strictly ordered.
    l1[0] = 0x00000000UL | PTE_DEVICE;

    // Second GiB: RAM, via an L2 table of 2 MiB Normal blocks.
    l1[1] = (uint64_t)l2 | PTE_VALID | PTE_TABLE;
    for (uint64_t off = 0; off < RAM_SIZE; off += BLOCK_2M)
        l2[off / BLOCK_2M] = (RAM_BASE + off) | PTE_NORMAL;

    uint64_t mair = MAIR_DEVICE_nGnRnE | (MAIR_NORMAL_WB << 8);

    __asm__ volatile(
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "isb\n"
        :: "r"(mair), "r"(TCR_VALUE), "r"((uint64_t)l1));

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0)     // M: MMU on
           | (1UL << 2)     // C: data cache on
           | (1UL << 12);   // I: instruction cache on
    __asm__ volatile(
        "dsb sy\n"          // all table writes complete before...
        "msr sctlr_el1, %0\n"
        "isb\n"             // ...and no instruction runs under old rules
        :: "r"(sctlr));

    kprintf("vm: MMU on, identity map, RAM=Normal MMIO=Device\n");
}
```

- [ ] **Step 3: Update `kernel/main.c`**

Replace the whole file — init order matters (`vm_init` needs `pmm_init`;
everything else keeps working through the identity map):

```c
#include "kprintf.h"
#include "trap.h"
#include "gic.h"
#include "timer.h"
#include "pmm.h"
#include "vm.h"

void kmain(void)
{
    exceptions_init();
    gic_init();
    pmm_init();
    vm_init();

    // Milestone 3 payoff: this exact read used to be a data abort.
    // RAM is Normal memory now -- unaligned access is legal.
    volatile unsigned int *p = (unsigned int *)0x40080001UL;
    kprintf("unaligned read at %p works now: %x\n", (void *)p, *p);

    timer_init(1);
    __asm__ volatile("msr daifclr, #2");
    kprintf("interrupts on; idling\n");

    for (;;)
        __asm__ volatile("wfi");
}
```

- [ ] **Step 4: Update the Makefile**

`OBJS` gains `build/vm.o`:

```make
OBJS := build/boot.o build/main.o build/uart.o build/kprintf.o \
        build/vectors.o build/trap.o build/gic.o build/timer.o \
        build/string.o build/pmm.o build/vm.o
```

Header deps gain `kernel/vm.h`:

```make
build/%.o: kernel/%.c kernel/uart.h kernel/kprintf.h kernel/trap.h \
           kernel/gic.h kernel/timer.h kernel/string.h kernel/pmm.h \
           kernel/vm.h | build
	$(CC) $(CFLAGS) -c $< -o $@
```

- [ ] **Step 5: Build and check output**

Run: `make` (no warnings), then:

```bash
qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 128M -display none \
    -serial file:/tmp/os-serial.log -kernel build/kernel.elf & QPID=$!
sleep 4; kill $QPID
cat /tmp/os-serial.log
```

Expected:

```
pmm: 32XXX pages free ...
pmm: a=... b=...
pmm: freed a, realloc'd -> ... (same page, LIFO works)
vm: MMU on, identity map, RAM=Normal MMIO=Device
unaligned read at 0x0000000040080001 works now: XXXXXXXX
interrupts on; idling
tick 1
tick 2
```

Must-check: the `vm: MMU on` line prints (kprintf works through the map = UART Device mapping correct), the unaligned read *succeeds* (RAM is Normal — the milestone 3 crash is gone), and ticks continue (GIC + timer survive the MMU).

- [ ] **Step 6: Commit**

```bash
git add kernel/vm.h kernel/vm.c kernel/main.c Makefile
git commit -m "milestone 5b: page tables, MMU on with Normal/Device attributes"
```

---

### Task 3: Milestone explainer doc

**Files:**
- Create: `docs/05-memory.md`

**Interfaces:**
- Consumes: code from Tasks 1–2.
- Produces: learning artifact in the established `docs/NN-topic.md` format.

- [ ] **Step 1: Write `docs/05-memory.md`**

Cover exactly these sections, expanded into teaching prose with code inlined:

1. **Two problems, one milestone** — (a) the kernel can't hand out RAM (no malloc, no page tables for future processes); (b) all memory is Device type: uncached (slow) and alignment-strict (the milestone 3 crash). The allocator solves (a); the MMU solves (b).
2. **The free-list trick** — free pages store the list through themselves: first 8 bytes of a free page = pointer to next free page. Zero metadata for 32K pages. LIFO behavior (the demo's `c == a`). What's deferred: double-free detection, coalescing — name them as known ceilings.
3. **The translation walk** — VA→PA via 3 levels (39-bit VA, 4 KiB granule): bits [38:30] index L1, [29:21] L2, [20:12] L3, [11:0] page offset. Draw the walk for VA `0x40080001`: L1[1] → table → L2[0] → 2 MiB block at 0x40000000 + offset. Blocks vs tables at each level: bit 1 chooses (block=leaf at L1/L2; L3 entries are always pages).
4. **Memory attributes: the indirection** — descriptors don't hold attributes, they hold a 3-bit *index* into `MAIR_EL1` (8 attribute slots). Ours: attr0 = `0x00` Device-nGnRnE, attr1 = `0xff` Normal write-back cacheable. Why Device for MMIO is *correctness* not performance: no caching (a cached UART write never leaves the CPU), no reordering, no speculation (a speculative read of GICC_IAR would *ack an interrupt by accident*).
5. **The AF bit and other landmines** — AF (bit 10): hardware refuses to use a mapping nobody marked "accessed" — clear AF = instant fault; the classic first-MMU-enable crash. `TCR_EL1` decoded field by field (T0SZ=25 → 39-bit, TG0, IRGN/ORGN/SH, EPD1 since we leave TTBR1 dead). The enable sequence choreography: `dsb sy` (table writes drain) → `msr sctlr` → `isb` (no stale fetches) — and why identity mapping makes turning the MMU on survivable: the next instruction fetch works because VA=PA.
6. **The milestone 3 payoff** — same unaligned read, opposite outcome, and *nothing about the read changed* — only the memory type of the region it touches. Attributes live in the page tables, not the instruction. Also: everything else (UART prints, GIC, timer ticks) kept working through the transition because identity mapping preserved every address the kernel knew.
7. **What we deferred** — higher-half kernel (kernel at `0xffff...`, user at `0x0000...` via TTBR1/TTBR0 split): why real OSes do it (kernel present in every address space, negative addresses unreachable from EL0 pointer bugs), why we don't need it yet (milestone 7 will protect the kernel with AP permission bits instead). One paragraph, honest about the trade.
8. **Try breaking it** — 3 experiments: delete `PTE_AF` from `PTE_NORMAL` (instant translation fault at the first RAM touch after enable — read the ESR: DFSC says Access-flag fault, level 2); map RAM as Device (`PTE_DEVICE` in the L2 loop) and watch the unaligned read crash *again* (attributes, not addresses, were the story all along); comment out the `isb` after `msr sctlr_el1` and see whether it still boots (it may! — then explain why "works in QEMU" ≠ "architecturally correct": TCG isn't a pipeline).

- [ ] **Step 2: Commit**

```bash
git add docs/05-memory.md
git commit -m "docs: milestone 5 explainer — page allocator, translation, MAIR"
```

---

## Self-review notes

- Spec coverage: milestone 5 = "physical page allocator (free-list), then MMU on: page tables, identity map, then higher-half kernel". Allocator = Task 1, page tables + identity map = Task 2, **higher-half deferred** — a deliberate, flagged deviation (doc section 7 explains it; nothing in milestones 6–7 depends on it; available as 5b if the user wants the full journey).
- Type consistency: `alloc_page()` (Task 1) consumed by `vm_init` (Task 2); `PAGE_SIZE` defined once in `pmm.h`.
- `-m 128M` appears in both the Makefile and every QEMU verification command; `RAM_END`/`RAM_SIZE` match it in pmm.c and vm.c.
- QEMU virt DTB confirmed earlier: RAM at 0x40000000; GIC/UART both under 1 GiB → covered by the Device block.
