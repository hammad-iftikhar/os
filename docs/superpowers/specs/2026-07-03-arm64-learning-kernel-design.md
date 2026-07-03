# ARM64 Learning Kernel — Design

**Date:** 2026-07-03
**Status:** Approved

## Purpose

A hobby operating system kernel built from scratch for learning. The process is the product: every milestone teaches a core OS concept and ends with observable, runnable behavior.

## Decisions

| Decision | Choice | Why |
|---|---|---|
| Architecture | ARM64 (AArch64) | Matches the user's Apple Silicon hardware conceptually |
| Target machine | QEMU `virt` board, cortex-a72 | Best-documented generic board: PL011 UART, GICv2/v3, no SD/GPU quirks; instant iteration |
| Language | C + minimal assembly | Keeps the machine visible; no extra language to learn |
| Toolchain | Apple clang + LLD, `--target=aarch64-elf`, freestanding | Already installed on macOS, zero setup; one compiler for the project |
| Runner | `qemu-system-aarch64` (Homebrew, installed) | `make run` boots the kernel |
| Work style | Claude writes code + thorough concept docs; user reads, runs, tweaks | Fastest path through the material |

## Repo structure

Flat, grows only when a milestone needs a file:

```
os/
├── Makefile          # make run = build + boot in QEMU; make clean
├── linker.ld
├── boot.S            # entry, BSS clear, stack setup, jump to kmain
├── kernel/           # .c/.h added per milestone
└── docs/             # NN-topic.md explainer per milestone
```

## Milestones

Each ends with something runnable in QEMU. One milestone at a time; the next starts only when the current one is understood.

1. **Boot & UART hello** — linker script, boot.S, raw PL011 writes; prints "Hello from kernel".
2. **kprintf & panic** — tiny printf (`%s %d %x %p %c`), `panic()`; the kernel can talk.
3. **Exceptions** — EL1 vector table, sync exception handler with register dump; deliberately crash and see it caught.
4. **Interrupts & timer** — GIC init, ARM generic timer, periodic tick printed.
5. **Memory** — physical page allocator (free-list), then MMU on: page tables, identity map, then higher-half kernel.
6. **Processes & scheduling** — kernel threads, context switch (x19–x30, sp), round-robin on the timer tick.
7. **Syscalls & user mode** — drop to EL0, `svc` syscall path, first user program.

Each milestone ships with `docs/NN-topic.md` explaining the concepts involved (exception levels, why the UART is at 0x0900_0000, ttbr0/ttbr1, etc.).

## Error handling

Panic loudly and early. A learning kernel crashes with a register dump; it never limps along. Milestone 3 exists early precisely to make all later mistakes debuggable.

## Testing

Observable QEMU behavior is the check for each milestone (printed output, tick messages, alternating thread output). No test framework.

## Out of scope (for now)

Filesystem, shell, SMP, real hardware (Raspberry Pi), networking. Any can be added after milestone 7 if desired.
