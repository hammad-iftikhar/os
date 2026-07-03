# Milestone 1: Boot & UART Hello — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boot a from-scratch ARM64 kernel on QEMU's `virt` board and print "Hello from kernel" over the PL011 UART.

**Architecture:** QEMU loads our ELF via `-kernel` and starts the CPU at EL1 at our entry point. `boot.S` parks secondary cores, clears BSS, sets the stack, and jumps to C. `kmain` prints via memory-mapped PL011 UART registers. A linker script places everything at 0x40080000 (RAM starts at 0x40000000; QEMU puts the device tree at the very start of RAM, so we link above it).

**Tech Stack:** Apple clang (`--target=aarch64-elf`, freestanding), `ld.lld` (Homebrew `lld`, installed, v22.1.8), `qemu-system-aarch64` (installed), GNU-syntax ARM64 assembly.

## Global Constraints

- Toolchain: `clang --target=aarch64-elf -ffreestanding -mgeneral-regs-only`; link with `ld.lld -T linker.ld` (no libc, no crt0).
- Target: `qemu-system-aarch64 -machine virt -cpu cortex-a72`.
- Error philosophy: panic loudly; never limp along (from spec).
- Commits: author is Hammad Iftikhar <hammad.iftikhar723@gmail.com> (repo config already set); NO Co-Authored-By trailer.
- Verified facts (do not re-derive): QEMU `virt` RAM base = 0x40000000, PL011 UART base = 0x09000000, kernel entered at EL1, QEMU's PL011 needs no init before TX.

## File Structure

```
os/
├── .gitignore            # build/
├── Makefile              # all / run / clean
├── linker.ld             # link at 0x40080000, defines __bss_start/__bss_end/__stack_top
├── boot.S                # _start: park non-boot cores, clear BSS, set SP, bl kmain
├── kernel/
│   ├── uart.h            # uart_putc / uart_puts
│   ├── uart.c            # PL011 MMIO implementation
│   └── main.c            # kmain: hello + wfe loop
└── docs/
    └── 01-boot-and-uart.md   # milestone explainer
```

---

### Task 1: Boot chain — linker script, boot.S, UART, kmain, Makefile

Nothing here is independently runnable — the first testable artifact is the booting kernel — so the whole chain is one task with one test cycle at the end.

**Files:**
- Create: `linker.ld`
- Create: `boot.S`
- Create: `kernel/uart.h`
- Create: `kernel/uart.c`
- Create: `kernel/main.c`
- Create: `Makefile`
- Create: `.gitignore`
- Test: observable QEMU serial output (per spec, no test framework)

**Interfaces:**
- Consumes: nothing (first code in the repo).
- Produces: `void uart_putc(char c)`, `void uart_puts(const char *s)` (used by every later milestone); linker symbols `__bss_start`, `__bss_end`, `__stack_top`; `make`, `make run`, `make clean`.

- [ ] **Step 1: Write `linker.ld`**

```ld
ENTRY(_start)

SECTIONS
{
    /* RAM starts at 0x40000000; QEMU places the DTB there. Link above it. */
    . = 0x40080000;

    .text : {
        KEEP(*(.text.boot))     /* _start must come first */
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(16);              /* AArch64 requires 16-byte-aligned SP */
    . += 0x4000;                /* 16 KiB boot stack */
    __stack_top = .;
}
```

- [ ] **Step 2: Write `boot.S`**

```asm
// Entry point. QEMU's -kernel loads this ELF and starts the boot CPU
// here at EL1, MMU off, caches off, x0 = physical address of the DTB.

.section .text.boot
.globl _start
_start:
    // Park every core except core 0 (harmless with -smp 1,
    // required the day we pass -smp 4).
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff           // Aff0 = core number within cluster
    cbnz    x0, halt

    // Clear BSS: C code assumes zeroed globals.
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b

2:  // C needs a stack.
    ldr     x0, =__stack_top
    mov     sp, x0
    bl      kmain

halt:                               // if kmain returns, or non-boot core
    wfe
    b       halt
```

- [ ] **Step 3: Write `kernel/uart.h`**

```c
#pragma once

void uart_putc(char c);
void uart_puts(const char *s);
```

- [ ] **Step 4: Write `kernel/uart.c`**

```c
#include <stdint.h>
#include "uart.h"

// PL011 UART on QEMU's virt board. QEMU wires it up pre-initialized,
// so TX is just: wait until FIFO not full, write the data register.
#define UART0_BASE  0x09000000UL
#define UARTDR      (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UARTFR      (*(volatile uint32_t *)(UART0_BASE + 0x18))
#define UARTFR_TXFF (1u << 5)   // transmit FIFO full

void uart_putc(char c)
{
    while (UARTFR & UARTFR_TXFF)
        ;
    UARTDR = (uint32_t)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');    // real terminals want CRLF
        uart_putc(*s++);
    }
}
```

- [ ] **Step 5: Write `kernel/main.c`**

```c
#include "uart.h"

void kmain(void)
{
    uart_puts("Hello from kernel\n");
    for (;;)
        __asm__ volatile("wfe");
}
```

- [ ] **Step 6: Write `Makefile`**

Note: recipe lines must be indented with a TAB, not spaces.

```make
CROSS  := --target=aarch64-elf
CC     := clang
CFLAGS := $(CROSS) -ffreestanding -mgeneral-regs-only -Wall -Wextra -O2 -g
LD     := ld.lld
QEMU   := qemu-system-aarch64 -machine virt -cpu cortex-a72

OBJS := build/boot.o build/main.o build/uart.o

all: build/kernel.elf

build:
	mkdir -p build

build/boot.o: boot.S | build
	$(CC) $(CROSS) -c $< -o $@

build/%.o: kernel/%.c kernel/uart.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/kernel.elf: $(OBJS) linker.ld
	$(LD) -T linker.ld $(OBJS) -o $@

run: build/kernel.elf
	$(QEMU) -nographic -kernel $<

clean:
	rm -rf build

.PHONY: all run clean
```

- [ ] **Step 7: Write `.gitignore`**

```gitignore
build/
```

- [ ] **Step 8: Build**

Run: `make`
Expected: no errors, no warnings; `build/kernel.elf` exists.

- [ ] **Step 9: Scripted output check**

`make run` is interactive (Ctrl-A X to exit), so verify non-interactively first:

```bash
qemu-system-aarch64 -machine virt -cpu cortex-a72 -display none \
    -serial file:/tmp/os-serial.log -kernel build/kernel.elf & QPID=$!
sleep 3; kill $QPID
cat /tmp/os-serial.log
```

Expected: `Hello from kernel`

- [ ] **Step 10: Interactive run (the real milestone moment)**

Run: `make run`
Expected: `Hello from kernel` in the terminal, kernel idles. Exit with **Ctrl-A X**.

- [ ] **Step 11: Commit**

```bash
git add .gitignore Makefile linker.ld boot.S kernel/
git commit -m "milestone 1: boot to C and print hello over PL011 UART"
```

---

### Task 2: Milestone explainer doc

**Files:**
- Create: `docs/01-boot-and-uart.md`

**Interfaces:**
- Consumes: the code from Task 1 (walks through it).
- Produces: the learning artifact for milestone 1; later docs follow its format (`docs/NN-topic.md`).

- [ ] **Step 1: Write `docs/01-boot-and-uart.md`**

Write the explainer covering exactly these sections, with these facts (expand each into clear teaching prose with the relevant code snippets from Task 1 inlined):

1. **The machine we're programming** — QEMU `virt` is an idealized ARM64 board. Memory map facts: RAM at `0x4000_0000`, PL011 UART MMIO at `0x0900_0000`, flash at `0x0`. QEMU's `-kernel` loads our ELF and jumps to `ENTRY(_start)`; the DTB (device tree blob, hardware description handed to the kernel) is placed at the start of RAM, its address passed in `x0` — which is why we link at `0x4008_0000`, above it.
2. **Exception levels** — EL0 (user) → EL1 (kernel) → EL2 (hypervisor) → EL3 (firmware). QEMU starts us at EL1: exactly where a kernel lives. MMU and caches are off; everything is physical addresses.
3. **Why `-ffreestanding`** — no libc, no `main()`, no crt0 startup; nothing exists until we create it. Also `-mgeneral-regs-only`: FP/SIMD registers trap at EL1 until enabled (CPACR_EL1), so the compiler must not emit them.
4. **The linker script** — walk through `linker.ld`: section placement, why `.text.boot` is KEEP'd first (entry must be the first byte QEMU jumps to), the `__bss_start`/`__bss_end`/`__stack_top` symbols as the contract between linker and `boot.S`, 16-byte SP alignment (AArch64 ABI hardware requirement — misaligned SP faults).
5. **boot.S line by line** — `mpidr_el1` identifies the core (park all but core 0); BSS must be zeroed because C guarantees zero-initialized globals; the stack must exist before any C call; `wfe` = wait-for-event, the polite way to idle a core.
6. **Talking to hardware: MMIO** — devices are registers at fixed physical addresses. `volatile` because each read/write has side effects the compiler must not optimize away or reorder. PL011: write bytes to `UARTDR`, poll `UARTFR.TXFF` (bit 5) so we never write to a full FIFO. `\r\n` because terminals distinguish carriage-return from line-feed.
7. **Try breaking it** — 3 suggested experiments: remove the BSS-clear loop and add a global `int x = 0` counter (observe garbage), link at `0x40000000` and see the DTB collision, drop `volatile` at `-O2` and watch prints vanish.

- [ ] **Step 2: Commit**

```bash
git add docs/01-boot-and-uart.md
git commit -m "docs: milestone 1 explainer — boot, exception levels, MMIO"
```

---

## Self-review notes

- Spec coverage: milestone 1 = boot + UART hello + explainer doc — Tasks 1 and 2 cover both deliverables. Makefile/`make run` required by spec — Task 1 Step 6.
- Toolchain, memory map, and QEMU invocation were verified empirically on this machine before writing this plan (smoke kernel printed over PL011).
- No TDD framework: per spec, observable QEMU output is the test (Steps 9–10).
