# Milestone 2: kprintf & panic

The kernel can now describe its own state. That sounds mundane — it's the single highest-leverage thing a young kernel can have, because every milestone after this one goes wrong in ways only formatted output can reveal.

## 1. Why write printf when printf exists

`printf` lives in libc, and `-ffreestanding` means there is no libc. Every convenience we're used to is code someone wrote — and down here, that someone is us. We could keep debugging with `uart_puts("got here 3\n")`, but the moment we need to inspect a register value or an address, string literals stop being enough. So before anything genuinely hard (exceptions, MMU), we build the tool that makes the hard things debuggable.

The kernel version is traditionally called `kprintf` — same idea as printf, minus everything we don't need: no field widths, no floats, no return value. Five specifiers from the spec (`%s %d %x %p %c`), plus `%%`.

## 2. Varargs without an OS

How does a function even accept "any number of arguments" with no OS underneath? The answer is that varargs never needed an OS — `<stdarg.h>` is provided by the **compiler**, not libc, so it works freestanding.

On ARM64 the calling convention passes the first 8 integer/pointer arguments in registers `x0`–`x7`, and the rest on the stack. `va_list` is a small structure that knows how to walk both regions; `va_arg(ap, int)` fetches the next argument and advances. The compiler generates all of it — we just use the macros:

```c
void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);   // "the varargs begin after fmt"
    vkprintf(fmt, ap);
    va_end(ap);
}
```

One rule worth internalizing: **default argument promotions**. Anything smaller than `int` (like `char`) is promoted to `int` when passed through `...`. That's why `%c` does `va_arg(ap, int)` and then casts down — asking for a `char` directly is undefined behavior.

## 3. Printing numbers backwards

Arithmetic hands you digits in the wrong order: `u % 10` gives the *last* digit first. So `print_uint` fills a buffer backwards, then emits it in reverse:

```c
char buf[20];            // largest u64 in decimal = 20 digits
int i = 0;
do {
    buf[i++] = digits[u % base];
    u /= base;
} while (u > 0);
while (i > 0)
    uart_putc(buf[--i]);
```

The `do/while` (not `while`) matters: it guarantees at least one digit, so `0` prints as `"0"` instead of nothing. And note what's absent — no allocation. There is no heap; there won't be one until milestone 5. Kernel code this early lives entirely on the stack.

## 4. The INT_MIN trap

The obvious way to print a negative number:

```c
if (n < 0) { uart_putc('-'); n = -n; }   // BUG
```

Looks fine, fails on exactly one input: `-2147483648`. Two's-complement int ranges from −2147483648 to +2147483647 — the positive counterpart of INT_MIN **does not exist**, so `-n` overflows, and signed overflow is undefined behavior in C. The fix is one line of ceremony:

```c
unsigned int u = (unsigned int)n;   // convert FIRST (well-defined)
if (n < 0) {
    uart_putc('-');
    u = -u;                          // unsigned negation: well-defined
}
```

Unsigned arithmetic wraps by definition, so `-u` on the converted INT_MIN yields exactly 2147483648. That's why the demo deliberately prints `int min: -2147483648` — it's the regression test for this trap.

## 5. Design choices that pay off later

Small decisions in this milestone were made with milestone 3 (exception handling) already in mind:

- **`vkprintf` exists separately** so `panic` can accept a format string too. Any future function that wants formatting (`log()`, `assert` messages) reuses the same core.
- **`%p` is fixed-width** — always `0x` + 16 hex digits. When we dump 31 registers after a crash, columns will align and differences will jump out.
- **Unknown specifiers print verbatim** (`%q` → `%q`). A typo in a format string announces itself instead of silently eating an argument.
- **`%s` with NULL prints `(null)`** instead of dereferencing it. The one place we bend "fail loudly": crashing *inside your debugging tool* while reporting another crash helps nobody.

## 6. panic(): fail loudly

The spec's error philosophy: a learning kernel must crash with information, never limp along. `panic` is that policy as code:

```c
_Noreturn void panic(const char *fmt, ...)
{
    kprintf("\nPANIC: ");
    /* ... vkprintf(fmt, ap) ... */
    for (;;)
        __asm__ volatile("wfe");
}
```

`_Noreturn` is a promise to the compiler — callers can treat `panic(...)` like a `return`, and the optimizer won't warn about code paths that "fall off the end" through it. `wfe` parks the core in a low-power doze forever. From now on, any kernel code that detects an impossible state calls `panic` with a message saying *what* and *where* — and in milestone 3, hardware faults will start doing the same thing automatically, with a full register dump.

## 7. Try breaking it

1. **Recreate the INT_MIN bug.** In `kprintf.c`, change the `%d` case to negate before converting (`if (n < 0) { uart_putc('-'); n = -n; } u = n;`). Build and run. It may *still print correctly* at `-O2` — that's the scary part of undefined behavior: it doesn't promise to fail. Change to `-O0` in the Makefile and compare. Code that works by accident is still broken.
2. **Remove the `\r` translation** in `vkprintf` (the `if (*fmt == '\n')` branch) and run `make run` in your terminal. Enjoy the staircase. This is the CRLF distinction terminals have kept since typewriter days.

## Where we go next

Milestone 3: right now, if the kernel touches a bad address, the CPU takes an exception to a vector table we haven't installed — and silently hangs. We'll build the EL1 vector table and a handler that prints exactly what happened, register by register. The kernel learns to catch its own crashes.
