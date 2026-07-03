# Milestone 5: Memory

The kernel now manages its own RAM and runs with the MMU on. And the proof is poetic: the exact unaligned read that crashed the kernel in milestone 3 now just... works. Nothing about the read changed — the *rules of memory itself* did.

## 1. Two problems, one milestone

Until now:

1. **The kernel couldn't hand out RAM.** No `malloc`, no way to give a future process a stack, no pages for page tables. All memory was either "part of the kernel image" or "unknown wilderness."
2. **All memory was Device type.** With the MMU off, every access is treated as strictly-ordered, uncached, alignment-fussy Device memory — correct for hardware registers, absurd for RAM. Slow, and it's why milestone 3's unaligned read faulted.

The **physical page allocator** (`pmm.c`) solves the first. The **MMU** (`vm.c`) solves the second. They meet in the middle: page tables live in pages the allocator hands out.

## 2. The free-list trick

The allocator manages ~32,000 pages of 4 KiB each, from `_kernel_end` (a new linker symbol marking the first byte past the kernel image and boot stack) to the top of RAM. Naively you'd build an array of 32,000 entries tracking what's free. The classic trick: **free pages store the list through themselves**.

```c
struct free_page {
    struct free_page *next;
};
```

A free page, by definition, holds nothing anyone cares about — so its first 8 bytes hold a pointer to the next free page. The bookkeeping for 130 MB of RAM costs zero bytes of dedicated metadata. `alloc_page` pops the head; `free_page` pushes onto it:

```c
void *alloc_page(void)          // pop
{
    struct free_page *p = free_list;
    if (!p)
        panic("pmm: out of memory");
    free_list = p->next;
    memset(p, 0, PAGE_SIZE);    // callers rely on zeroed pages
    return p;
}
```

It's LIFO — the demo shows freeing page `a` and immediately reallocating returns exactly `a`. Two deliberate ceilings, named per house rules: no double-free detection and no coalescing of adjacent pages (we never allocate multi-page runs yet). Both are afternoon upgrades when a milestone needs them.

Also new: `string.c` with `memset`/`memcpy`. Freestanding code must supply these — and subtly, the *compiler* assumes they exist too: clang will happily lower a struct zeroing into a `memset` call even at `-ffreestanding`.

## 3. The translation walk

With the MMU on, every address the CPU touches is **virtual**, translated to physical via page tables. We chose a 39-bit address space with 4 KiB pages, which means a 3-level walk. The VA's bits are the map:

```
VA bits:  [38:30]      [29:21]      [20:12]      [11:0]
          L1 index     L2 index     L3 index     offset in page
          (1 GiB each) (2 MiB each) (4 KiB each)
```

Each table is exactly one 4 KiB page holding 512 8-byte descriptors. A descriptor at L1/L2 can be a **table** pointer (bit 1 = 1, walk continues) or a **block** (bit 1 = 0, translation ends here with a huge page). Our whole map is five lines of code:

```c
l1[0] = 0x00000000UL | PTE_DEVICE;          // 1 GiB block: all MMIO
l1[1] = (uint64_t)l2 | PTE_VALID | PTE_TABLE;
for (uint64_t off = 0; off < RAM_SIZE; off += BLOCK_2M)
    l2[off / BLOCK_2M] = (RAM_BASE + off) | PTE_NORMAL;  // 64 x 2 MiB
```

Walk `0x40080001` (the demo address) by hand: bits [38:30] = 1 → `l1[1]` → table, follow to L2. Bits [29:21] = 0 → `l2[0]` → 2 MiB Normal block at `0x40000000`. Remaining bits are the offset: `0x80001`. Physical = `0x40080001`. VA equals PA — that's what **identity mapping** means. Everything the kernel knew before the switch (its own code, the stack, the UART, the GIC) stays at the same address, which is exactly why the switch is survivable: the instruction *after* "MMU on" fetches from the same address it would have anyway.

## 4. Memory attributes: the indirection

Here's the part that actually fixed the crash. Descriptors don't encode memory attributes directly — they hold a 3-bit **index** into `MAIR_EL1`, a register holding 8 attribute recipes:

```c
uint64_t mair = MAIR_DEVICE_nGnRnE | (MAIR_NORMAL_WB << 8);
//              attr0 = 0x00: Device    attr1 = 0xff: Normal, write-back cached
```

- **Normal** (RAM): cacheable, reorderable, speculatable, unaligned-access-friendly. The CPU may combine writes, prefetch, keep hot lines in L1.
- **Device-nGnRnE** (MMIO): none of that. And this is *correctness*, not performance tuning. A cached UART write might never leave the CPU. A *speculatively executed* read of `GICC_IAR` would **acknowledge an interrupt by accident** — reads with side effects must never happen "just in case." Device type forbids all of it.

Milestone 3's fault, re-explained: with the MMU off, *everything* — including RAM — gets Device treatment, and Device memory architecturally requires aligned access. Turn the MMU on, mark RAM as Normal, and the same unaligned load is legal. The instruction didn't change; its memory's *type* did.

## 5. The AF bit and other landmines

Getting the MMU to turn on without an instant crash is a rite of passage. The classic traps, all present in our code:

- **The Access Flag** (`PTE_AF`, bit 10). Hardware refuses to use a mapping whose AF is clear — you get a fault on first touch. It exists so OSes can detect which pages are actually used (for swapping). We don't do that, so every descriptor sets AF at creation. Forget it and the first RAM access after enable faults.
- **`TCR_EL1`** — the walk configuration, decoded: `T0SZ=25` (64−25 = 39-bit VA), `TG0=0` (4 KiB granule), `IRGN0/ORGN0=1` + `SH0=3` (table walks themselves are cacheable, inner-shareable), `EPD1=1` (we don't use TTBR1 — walks through it disabled dead, rather than left pointing at garbage), `IPS=2` (40-bit physical).
- **The enable choreography**:

```c
"dsb sy\n"              // every table write is in memory before...
"msr sctlr_el1, %0\n"   // ...the MMU starts reading tables
"isb\n"                 // ...and no already-fetched instruction runs by old rules
```

`dsb` orders memory, `isb` flushes the pipeline. Skip them and you're at the mercy of what happened to be in flight.

## 6. The milestone 3 payoff

```
unaligned read at 0x0000000040080001 works now: d53800
```

Bonus detail: that value isn't random. `0x40080000` holds the first instruction of `boot.S` — `mrs x0, mpidr_el1`, which encodes as `0xd5380000`. Read 4 bytes starting one byte in, and you get `0x00d53800`. The kernel is reading its own first instruction, off by one byte — *legally*, because RAM is Normal memory now.

And check what *didn't* change: `kprintf` printed through the whole transition (UART correctly mapped Device), and the timer ticks on (GIC likewise). Identity mapping made the MMU flip invisible to every other subsystem.

## 7. What we deferred: the higher-half kernel

Real OSes don't identity-map. They put the kernel at the top of the address space (`0xffff...`, via `TTBR1`) and user programs at the bottom (`0x0000...`, via `TTBR0`) — the kernel is then present in every process's address space (syscalls don't switch tables), and kernel addresses are unreachable garbage from a user pointer bug. We skipped it: it requires relinking the kernel at a high VA and a delicate jump across the switch, and nothing in milestones 6–7 needs it — milestone 7 will protect the kernel with permission bits (AP) in the descriptors instead. It's the one real deviation from the original spec, available as "milestone 5b" if you want the full journey later.

## 8. Try breaking it

1. **Delete `PTE_AF` from `PTE_NORMAL`.** Instant fault after enable. Read the dump: ESR's DFSC field says *Access flag fault, level 2* — the hardware tells you which level of the walk objected.
2. **Map RAM as Device** — use `PTE_DEVICE` in the L2 loop. The kernel boots (slower), and the unaligned read crashes *again*. Attributes, not addresses, were the story all along.
3. **Remove the `isb` after `msr sctlr_el1`.** It probably still boots! QEMU's TCG isn't a real pipeline; there are no in-flight instructions to go stale. This is the most important lesson in the milestone: **"works in QEMU" and "architecturally correct" are different claims.** Real silicon would eventually eat you.

## Where we go next

Milestone 6: processes. `alloc_page` can now hand every kernel thread its own stack; the timer tick becomes a scheduler's heartbeat; and the trap-frame save/restore from milestone 4 learns the one new trick that makes multitasking real — restoring a *different* frame than the one that was saved.
