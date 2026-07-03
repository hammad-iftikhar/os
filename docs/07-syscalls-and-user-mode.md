# Milestone 7: Syscalls & User Mode

The last milestone of the roadmap, and the one that changes what this project *is*. A program now runs at EL0 — where the hardware, not our code, forbids it from touching kernel memory, devices, or system registers. It talks to the kernel through exactly one door (`svc`), gets preempted by the same timer as everyone else, and when it reaches for kernel memory anyway, the MMU catches it red-handed. That's not a program juggling functions anymore. That's an operating system running a program.

## 1. What "user mode" buys

Everything before this milestone ran at EL1 — any line of code could scribble on page tables, reprogram the GIC, or halt the machine. Trust was total because everything was the kernel.

EL0 inverts that. Code at EL0 *architecturally cannot*: access memory whose page descriptors don't grant EL0 permission, execute privileged instructions (`mrs`/`msr` of system registers trap), or mask interrupts to hog the CPU. It doesn't matter what instructions the user program contains — the hardware checks every memory access and every instruction against the current EL. The kernel stops being "the whole program" and becomes a **service with a hardware-enforced API**: the syscall table is the entire attack surface.

## 2. Building the user's world

The user program has no filesystem to be loaded from (`ponytail:` ceiling — an ELF loader when there's a disk), so it lives inside the kernel image as a self-contained, position-independent assembly blob in a `.user_code` section. `user_run` gives it a world of exactly two pages:

| Page | VA | Permissions | Bits |
|---|---|---|---|
| code | `0x8000_0000` | EL0 read+execute, EL1 read-only, EL1 never executes | `AP=11`, UXN clear, **PXN** |
| stack | `0x8000_1000` | EL0+EL1 read/write, nobody executes | `AP=01`, **UXN**, **PXN** |
| all kernel RAM | `0x4000_0000`+ | EL0: nothing | `AP=00` |

VA `0x8000_0000` is L1 slot 2 — an address range the identity map never touched, so `vm_map_user` builds a fresh L2→L3 chain with `alloc_page` (this is why milestone 5 taught the walk: now we *construct* one on demand).

Two of these bits are security decisions worth pausing on. **UXN** on the stack means a classic "inject code into a buffer and jump to it" attack dies as an instruction abort. **PXN** on user code closes the reverse door: if the *kernel* is ever tricked into jumping to a user-controlled address, it must not execute user-chosen instructions with EL1 privilege — that's a privilege escalation with a long real-world rap sheet.

## 3. The cache-maintenance wart

`user_run` copies instructions with `memcpy` — through the *data* cache. The instruction fetcher has its own path and could see stale bytes:

```c
__asm__ volatile("dc cvau, %0\n dsb ish\n ic iallu\n dsb ish\n isb" :: "r"(code));
```

Clean the data cache to the point of unification, invalidate the instruction cache, synchronize. QEMU's TCG doesn't model split caches, so this works without — same lesson as milestone 5's `isb`: **"works in QEMU" is not "architecturally correct."** Skip it on real silicon and you'll eventually execute garbage, but only sometimes, only under load, only on Tuesdays.

## 4. eret down, svc up

The descent is four instructions:

```asm
enter_user:
    msr     sp_el0, x1          // EL0 has its OWN stack pointer register
    msr     elr_el1, x0         // "return" address: the user entry point
    msr     spsr_el1, xzr       // EL0t, all interrupt masks clear
    eret
```

We forge the state a trap *would* have saved, then "return" from a trap that never happened (milestone 6 forged a return address to birth a thread; same move, now crossing a privilege boundary). `SP_EL0` being a separate register is elegant: when any trap later fires, the CPU switches to `SP_EL1` automatically — the kernel always lands on its own stack, and the user can't sabotage that by wrecking theirs.

The ascent is the mirror. `svc #0` at EL0 → vector 8 (`sync EL0/64`) → the trap frame machinery from milestone 3, unchanged → `handle_sync_el0`:

```c
if (ec == 0x15) {                   // SVC: a syscall
    tf->x[0] = syscall_dispatch(tf);
    return;                         // restore + eret resumes after the svc
}
```

Three details doing quiet work: the hardware set ELR to the instruction *after* the `svc` (unlike a fault, which points *at* the faulting instruction — you retry faults, you don't retry syscalls); the syscall number rides in the trap frame's `x[8]` and args in `x[0..5]` (our ABI, Linux-flavored); and writing `tf->x[0]` is how the return value materializes in the user's world — the restore path loads it into the real x0 before `eret`.

## 5. The trust boundary

`sys_write` receives a pointer from EL0. Here is the most important sentence in this milestone: **a user pointer is just an integer someone untrusted chose.** If the kernel dereferences it blindly, the user program can make the kernel read (and print!) any kernel address — passing `0x40080000` to `write` would *succeed* where the direct `ldr` faults, because the kernel is allowed to read everything. So:

```c
if (uptr < USER_CODE_VA || uptr >= USER_STACK_TOP) {
    kprintf("syscall: write with bad pointer %p, rejected\n", (void *)uptr);
    return (uint64_t)-1;
}
```

...and the string walk is bounded three ways (NUL, end of user memory, 256 bytes) so a string that "never ends" can't walk the kernel off the end of the user's pages.

Note the philosophical shift in `syscall_dispatch`'s default case: unknown syscall numbers return `-1` instead of panicking. Our fail-loud philosophy has governed every milestone — and it yields exactly here, at the trust boundary, to a different principle: **never let the untrusted side choose when you die.** A user bug is not a kernel bug. (User *faults* still end in a panic-dump because we have one user program and no way to kill just it — teardown arrives when processes do.)

## 6. One scheduler, two worlds

```
[user] hello from EL0 via svc
[user] hello from EL0 via svc
  K (kernel thread) says 1
[user] hello from EL0 via svc
```

Kernel thread K interleaving with EL0 greetings — this cost *zero new scheduler code*. Vector 9 (IRQ from EL0) routes to the same `irq_stub` as vector 5; the trap frame's SPSR remembers which world was interrupted, so the final `eret` returns there. The main thread became the user program's "vehicle": every trap from EL0 lands on its kernel stack, and when `schedule()` switches it out mid-trap, the user program is switched out with it. EL0 code is preempted, scheduled, and resumed by machinery that doesn't know it exists.

## 7. The finale, decoded

```
*** EXCEPTION: sync EL0/64 ***
ESR  = 0x000000009200000e  EC=24: data abort, lower EL
ELR  = 0x0000000080000034  (faulting instruction)
FAR  = 0x0000000040080000  (faulting address)
```

Read it like a detective: **`sync EL0/64`** — vector 8, the "lower EL" row of the table, installed in milestone 3 and dormant until today. **`EC=24`** — compare milestone 3's `EC=25`: the same data abort, one bit different, and that bit means "the *lower* EL did it" — the hardware distinguishes "kernel bug" from "user caught" in the syndrome itself. **`ELR = 0x80000034`** — a *user* address; the crime scene is in user space. **`FAR = 0x40080000`** — the kernel address it reached for. And the fault subtype (DFSC `0x0e`, permission fault level 2) says the walk *found* a valid mapping and the AP bits said no. Nothing in our C code checked anything — protection is a property of the page tables, enforced by silicon on every single access.

## 8. Try breaking it

1. **Attack through the syscall**: change `uprog.S` to pass `0x40080000` as the pointer to `SYS_write`. The validation rejects it politely. Now delete the range check in `sys_write` and run again: the kernel happily prints kernel memory — you've built an arbitrary-read gadget. Restore the check with new respect.
2. **Syscall 99**: `mov x8, #99; svc #0`. Polite `-1`, kernel unbothered. Compare: before this milestone, an unknown code path meant panic.
3. **Executable stack**: remove `PTE_UXN` from the `exec=0` path in `vm_map_user`, have the user program write a `ret` instruction's bytes to its stack and `blr` to it — it executes. Put UXN back: instruction abort. You've just toggled the mitigation that ended the shellcode era.

## 9. Where the roadmap ends, and what's beyond

Seven milestones, each one visible in the serial output of the last boot:

1. **Boot & UART** — linker script, boot.S, three preconditions for C, MMIO. *It speaks.*
2. **kprintf & panic** — varargs, number printing, fail-loud. *It describes itself.*
3. **Exceptions** — vector table, trap frames, ESR forensics. *It catches its own crashes.*
4. **Interrupts & timer** — GIC, generic timer, eret. *It has a heartbeat, and survives being interrupted.*
5. **Memory** — page allocator, translation walk, Normal vs Device. *It owns its RAM, and the milestone-3 crash stopped crashing.*
6. **Threads** — swtch, preemption, the first critical section. *It runs many things at once, none of them willingly.*
7. **User mode** — EL0, syscalls, the trust boundary. *It runs programs it doesn't trust, safely.*

Beyond the original spec, in rough order of payoff: **multiple user processes** (a `struct process` owning its own TTBR0 root — switch translation tables in `swtch` and each program gets a private address space); **a filesystem + ELF loader** (virtio-blk on QEMU, then programs stop living inside the kernel image); **higher-half kernel** (the deferred 5b); **SMP** (the cores we parked in boot.S's first four instructions wake up — and every data structure in this kernel learns what locks are for); **real hardware** (a Raspberry Pi port: same concepts, real UART init, real cache maintenance bugs).

The kernel is ~1,200 lines. Every one of them is yours to break.
