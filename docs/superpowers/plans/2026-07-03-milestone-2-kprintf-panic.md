# Milestone 2: kprintf & panic — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the kernel a voice — `kprintf` with `%s %d %x %p %c %%` and a `panic()` that prints a formatted message and halts.

**Architecture:** One new module `kernel/kprintf.c` built on milestone 1's `uart_putc`/`uart_puts`. A shared `vkprintf(fmt, va_list)` core does the formatting; `kprintf` and `panic` are thin wrappers over it. `panic` prints `PANIC: <message>` and parks the core forever. `kmain` becomes a demo that exercises every format specifier and ends in a deliberate panic — the observable output is the test.

**Tech Stack:** Same as milestone 1: clang `--target=aarch64-elf` freestanding, `ld.lld`, QEMU virt. `<stdarg.h>` is compiler-provided and works freestanding.

## Global Constraints

- Toolchain: `clang --target=aarch64-elf -ffreestanding -mgeneral-regs-only`; link with `ld.lld -T linker.ld` (no libc).
- Target: `qemu-system-aarch64 -machine virt -cpu cortex-a72`.
- Error philosophy: panic loudly; never limp along (from spec).
- Commits: author Hammad Iftikhar <hammad.iftikhar723@gmail.com> (repo config set); NO Co-Authored-By trailer.
- Build must stay warning-free under `-Wall -Wextra`.

## File Structure

```
kernel/
├── kprintf.h    # NEW: kprintf, vkprintf, panic declarations
├── kprintf.c    # NEW: formatting core + panic
├── main.c       # MODIFIED: demo of every specifier + deliberate panic
├── uart.h       # unchanged (consumed)
└── uart.c       # unchanged
Makefile         # MODIFIED: add build/kprintf.o
```

---

### Task 1: kprintf, panic, and the demo kmain

Formatting logic and its only possible test harness (QEMU output) land together.

**Files:**
- Create: `kernel/kprintf.h`
- Create: `kernel/kprintf.c`
- Modify: `kernel/main.c` (replace whole file)
- Modify: `Makefile` (OBJS line and C-object rule dependency)
- Test: observable QEMU serial output (per spec)

**Interfaces:**
- Consumes: `void uart_putc(char c)`, `void uart_puts(const char *s)` from `kernel/uart.h` (milestone 1).
- Produces: `void kprintf(const char *fmt, ...)`, `void vkprintf(const char *fmt, va_list ap)`, `_Noreturn void panic(const char *fmt, ...)` — used by every later milestone (exception dumps in milestone 3 depend on `%x`/`%p` and `panic`).

Supported specifiers (from spec): `%s %d %x %p %c` plus `%%` for a literal percent. Unknown specifiers print verbatim (e.g. `%q` → `%q`) so a typo is visible, not invisible.

- [ ] **Step 1: Write `kernel/kprintf.h`**

```c
#pragma once

#include <stdarg.h>

void kprintf(const char *fmt, ...);
void vkprintf(const char *fmt, va_list ap);
_Noreturn void panic(const char *fmt, ...);
```

- [ ] **Step 2: Write `kernel/kprintf.c`**

```c
#include <stdarg.h>
#include <stdint.h>
#include "kprintf.h"
#include "uart.h"

// Print u in the given base, no allocation: fill a buffer backwards.
// 64-bit worst case: 20 decimal digits.
static void print_uint(uint64_t u, unsigned base)
{
    static const char digits[] = "0123456789abcdef";
    char buf[20];
    int i = 0;

    do {
        buf[i++] = digits[u % base];
        u /= base;
    } while (u > 0);

    while (i > 0)
        uart_putc(buf[--i]);
}

void vkprintf(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (*fmt == '\n')
                uart_putc('\r');
            uart_putc(*fmt);
            continue;
        }

        fmt++;                          // skip the '%'
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            uart_puts(s ? s : "(null)");
            break;
        }
        case 'd': {
            int n = va_arg(ap, int);
            unsigned int u = (unsigned int)n;
            if (n < 0) {
                uart_putc('-');
                u = -u;                 // safe even for INT_MIN
            }
            print_uint(u, 10);
            break;
        }
        case 'x':
            print_uint(va_arg(ap, unsigned int), 16);
            break;
        case 'p': {
            // fixed-width: pointers align in register dumps
            uintptr_t p = (uintptr_t)va_arg(ap, void *);
            uart_puts("0x");
            for (int shift = 60; shift >= 0; shift -= 4)
                uart_putc("0123456789abcdef"[(p >> shift) & 0xf]);
            break;
        }
        case 'c':
            uart_putc((char)va_arg(ap, int));   // char promotes to int
            break;
        case '%':
            uart_putc('%');
            break;
        case '\0':
            return;                     // format ends in a lone '%'
        default:                        // unknown specifier: show it
            uart_putc('%');
            uart_putc(*fmt);
            break;
        }
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
}

_Noreturn void panic(const char *fmt, ...)
{
    va_list ap;

    kprintf("\nPANIC: ");
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");

    for (;;)
        __asm__ volatile("wfe");
}
```

- [ ] **Step 3: Replace `kernel/main.c` with the demo**

```c
#include "kprintf.h"

void kmain(void)
{
    kprintf("Hello from kernel\n");
    kprintf("string: %s, char: %c, percent: %%\n", "works", 'A');
    kprintf("decimal: %d, negative: %d, int min: %d\n",
            42, -42, -2147483648);
    kprintf("hex: %x, pointer: %p\n", 0xdeadbeef, (void *)kmain);
    kprintf("unknown specifier passes through: %q\n");

    panic("end of milestone 2 demo (deliberate)");
}
```

- [ ] **Step 4: Add `kprintf.o` to the Makefile**

Change the `OBJS` line:

```make
OBJS := build/boot.o build/main.o build/uart.o build/kprintf.o
```

And widen the C-object rule's header dependency (kprintf.h now exists):

```make
build/%.o: kernel/%.c kernel/uart.h kernel/kprintf.h | build
	$(CC) $(CFLAGS) -c $< -o $@
```

- [ ] **Step 5: Build**

Run: `make`
Expected: compiles with no warnings; links `build/kernel.elf`.

- [ ] **Step 6: Scripted output check**

```bash
qemu-system-aarch64 -machine virt -cpu cortex-a72 -display none \
    -serial file:/tmp/os-serial.log -kernel build/kernel.elf & QPID=$!
sleep 3; kill $QPID
cat /tmp/os-serial.log
```

Expected output (pointer value will differ but must be `0x` + 16 hex digits starting `00000000400`):

```
Hello from kernel
string: works, char: A, percent: %
decimal: 42, negative: -42, int min: -2147483648
hex: deadbeef, pointer: 0x0000000040080XXX
unknown specifier passes through: %q

PANIC: end of milestone 2 demo (deliberate)
```

- [ ] **Step 7: Commit**

```bash
git add kernel/kprintf.h kernel/kprintf.c kernel/main.c Makefile
git commit -m "milestone 2: kprintf (%s %d %x %p %c) and panic"
```

---

### Task 2: Milestone explainer doc

**Files:**
- Create: `docs/02-kprintf-and-panic.md`

**Interfaces:**
- Consumes: the code from Task 1 (walks through it).
- Produces: learning artifact; follows the `docs/NN-topic.md` format from milestone 1.

- [ ] **Step 1: Write `docs/02-kprintf-and-panic.md`**

Write the explainer covering exactly these sections, expanding each into teaching prose with code snippets from Task 1 inlined:

1. **Why write printf when printf exists** — freestanding means no libc; every convenience is code we write. A kernel without formatted output is undebuggable, so this comes before anything hard (exceptions, MMU).
2. **Varargs without an OS** — `<stdarg.h>` is provided by the *compiler*, not libc, so it works freestanding. On ARM64, `va_arg` reads the calling convention: first 8 integer args arrive in registers x0–x7, the rest on the stack; `va_list` walks both. Note the promotion rule: `%c` fetches an `int` because C promotes char through `...`.
3. **Printing numbers backwards** — you discover digits least-significant-first (`u % base`), so fill a buffer backwards and emit it in reverse. 20 bytes covers the largest 64-bit decimal. No allocation — there is no heap yet.
4. **The INT_MIN trap** — `-INT_MIN` overflows (undefined behavior) in signed arithmetic; converting to unsigned *first* and negating there is well-defined. This is why the demo prints `-2147483648`.
5. **Design choices that pay off later** — `vkprintf` split so `panic` can reuse formatting; `%p` fixed-width (16 hex digits) so milestone 3's register dumps align in columns; unknown specifiers echoed verbatim so typos are visible; `%s` prints `(null)` instead of crashing on NULL.
6. **panic(): fail loudly** — the spec's philosophy: a learning kernel must crash with information, never limp. `_Noreturn` tells the compiler (callers can rely on no fallthrough), `wfe` parks the core. Milestone 3 upgrades panic-adjacent output with full register dumps.
7. **Try breaking it** — 2 experiments: print `%d` with `-2147483648` after changing the code to negate *before* the unsigned conversion (UB — may still "work" at -O2, which is the scary part); remove the `\r` translation and run `make run` to see staircase output in a real terminal.

- [ ] **Step 2: Commit**

```bash
git add docs/02-kprintf-and-panic.md
git commit -m "docs: milestone 2 explainer — varargs, number printing, panic"
```

---

## Self-review notes

- Spec coverage: milestone 2 = "tiny printf (`%s %d %x %p %c`), `panic()`" — Task 1 implements exactly those plus `%%`; Task 2 is the required explainer.
- Type consistency: `vkprintf(const char *, va_list)` matches between header, implementation, and both callers.
- The demo `kmain` ends in a deliberate panic; milestone 3 replaces `kmain` anyway (it will install the vector table and trigger a real exception).
