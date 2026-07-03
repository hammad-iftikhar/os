# Milestone 1: Boot & UART Hello

What it takes to go from "QEMU starts a CPU" to "our C code prints a string" — with nothing underneath us. No BIOS services, no libc, no operating system. We *are* the operating system.

## 1. The machine we're programming

QEMU's `virt` board is an idealized ARM64 computer — no real-world quirks, just the essentials at fixed physical addresses:

| Address | What lives there |
|---|---|
| `0x0000_0000` | Flash (we don't use it) |
| `0x0900_0000` | PL011 UART (our serial port) |
| `0x4000_0000` | Start of RAM |

When we run `qemu-system-aarch64 -kernel build/kernel.elf`, QEMU loads our ELF into RAM and jumps to its entry point (`ENTRY(_start)` in the linker script). One catch: QEMU also copies the **DTB** — the *device tree blob*, a data structure describing the hardware (how much RAM, where devices are) — to the very start of RAM at `0x4000_0000`, and passes its address to us in register `x0`. That's why we link our kernel at `0x4008_0000`: half a megabyte up, safely above the DTB. Later milestones can parse it; for now we just avoid stomping on it.

## 2. Exception levels: where we are in the privilege hierarchy

ARM64 has four privilege rings, called **exception levels**:

```
EL3  firmware / secure monitor     (most privileged)
EL2  hypervisor
EL1  operating system kernel       ← we are here
EL0  user programs                 (least privileged)
```

QEMU's `-kernel` drops us straight into **EL1** — exactly where a kernel belongs. The MMU is off and caches are off, so every address in our code is a *physical* address. Virtual memory doesn't exist until we build it (milestone 5).

## 3. Why `-ffreestanding` and `-mgeneral-regs-only`

A normal C program is *hosted*: libc exists, `main()` is called by startup code, `printf` works. We have none of that — our environment is *freestanding*:

- **`-ffreestanding`** tells clang: no libc, no assumptions that standard functions exist. Nothing exists until we write it.
- **`-mgeneral-regs-only`** tells clang: never emit floating-point/SIMD instructions. At EL1, FP/SIMD registers **trap** (fault) until explicitly enabled via the `CPACR_EL1` register. The optimizer loves using SIMD for things like copying structs — one stray `str q0, [...]` and we'd crash before printing a single byte.

## 4. The linker script: deciding where everything lives

The compiler produces relocatable code; the **linker script** (`linker.ld`) pins it to real addresses:

```ld
ENTRY(_start)
SECTIONS
{
    . = 0x40080000;             /* everything starts here, above the DTB */

    .text : {
        KEEP(*(.text.boot))     /* _start must come first */
        *(.text*)
    }
    .rodata : { *(.rodata*) }   /* string literals like "Hello from kernel" */
    .data   : { *(.data*) }    /* initialized globals */
    .bss    : {
        __bss_start = .;
        *(.bss*)                /* zero-initialized globals */
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(16);
    . += 0x4000;                /* 16 KiB boot stack */
    __stack_top = .;
}
```

Things to notice:

- **`KEEP(*(.text.boot))` is first.** QEMU jumps to our ELF entry; `boot.S` puts `_start` in a section called `.text.boot`, and the script guarantees it's the first thing in memory. `KEEP` stops the linker from discarding it as "unreferenced."
- **`__bss_start`, `__bss_end`, `__stack_top` are linker symbols** — addresses computed at link time. They're the contract between the linker and `boot.S`: assembly reads them to know what to zero and where to point the stack.
- **`ALIGN(16)`**: AArch64 *hardware* requires the stack pointer be 16-byte aligned — using a misaligned `sp` faults. Not a convention, a fault.
- The stack is just 16 KiB of reserved address space. Stacks grow *downward*, so we point `sp` at the top.

## 5. boot.S, line by line

C code has three preconditions: only one core running it, zeroed globals, and a stack. `boot.S` establishes all three:

```asm
_start:
    mrs     x0, mpidr_el1       // read the core-ID register
    and     x0, x0, #0xff       // Aff0 field = core number
    cbnz    x0, halt            // every core except 0: go sleep
```

`MPIDR_EL1` is a system register (read with `mrs`) identifying which core is executing. With `-smp 1` there's only core 0, but the day we boot 4 cores, all 4 would race through our boot code and take turns corrupting the same stack. Cores 1–3 get parked at `halt`.

```asm
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.hs    2f                  // branch if x0 >= x1 (unsigned)
    str     xzr, [x0], #8       // store zero, advance pointer by 8
    b       1b
```

C guarantees that `static int counter;` starts at zero — but nobody zeroes RAM for us. The compiler put all such variables in `.bss` and *assumes* it's zeroed. This loop makes that true, 8 bytes at a time (`xzr` is the always-zero register).

```asm
2:  ldr     x0, =__stack_top
    mov     sp, x0
    bl      kmain
halt:
    wfe
    b       halt
```

Point `sp` at the stack top, call into C. If `kmain` ever returns (it shouldn't), fall into `halt`: `wfe` — *wait for event* — puts the core into a low-power doze instead of spinning at 100%.

## 6. Talking to hardware: memory-mapped I/O

There are no "device drivers" to call. Devices *are memory addresses*: the PL011 UART is a bank of 32-bit registers starting at `0x0900_0000`. Store a byte to the data register, and it comes out of the serial port:

```c
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
```

- **`volatile` is load-bearing.** It tells the compiler each read/write has side effects: don't cache it, don't delete it, don't reorder it. Without it, the optimizer sees "write to the same address repeatedly, never read back" and deletes all but the last write. Your prints silently vanish.
- **Poll `UARTFR.TXFF` before writing** — the *flag register*'s bit 5 means "transmit FIFO full." QEMU drains the FIFO instantly so this never blocks here, but on real silicon it's the difference between working output and dropped bytes.
- **`\n` becomes `\r\n`** in `uart_puts` because terminals treat them differently: line feed moves down, carriage return moves to column 0. Raw `\n` output gives you staircase text on real terminals.

QEMU is forgiving: its PL011 works with zero initialization. Real hardware would need baud rate and line-format setup first — that's deliberately out of scope until it matters.

## 7. Try breaking it

The fastest way to make this stick — each takes one edit and `make run`:

1. **Skip the BSS clear.** Comment out the loop in `boot.S`, add `static int counter;` to `main.c` and print whether it's zero. On QEMU fresh RAM is usually zero anyway (anticlimactic); on real hardware it's garbage. Now you know why the loop exists *and* why this bug class is nasty — it hides.
2. **Link at `0x40000000`.** Change the linker script and watch the kernel and DTB fight over the same RAM. (QEMU may skip loading the DTB, or your kernel gets corrupted — either way, undefined weirdness.)
3. **Remove `volatile`.** Drop it from `UARTDR`/`UARTFR` and build with `-O2`. Watch your output disappear entirely. Put it back. Never forget it again.

## Where we go next

Milestone 2: `uart_putc` is our only voice, and it only speaks characters. Next we build `kprintf` (`%s %d %x %p`) and `panic()` so the kernel can describe its own state — which we'll need desperately when we start catching exceptions in milestone 3.
