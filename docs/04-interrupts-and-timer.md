# Milestone 4: Interrupts & Timer

The kernel now has a heartbeat. Once per second, hardware interrupts whatever is happening, the kernel prints a tick, and the interrupted code resumes as if nothing occurred. Small output, big change: this is the first time the kernel *survives* an exception — and that survival machinery is most of what a scheduler needs.

## 1. Polling vs interrupts

Until now the kernel could only *do* things, top to bottom. If it wanted to react to hardware it would have to sit in a loop asking "anything yet? anything yet?" — burning 100% CPU to mostly hear "no".

Interrupts invert control: the kernel goes to sleep and the hardware taps it on the shoulder. Look at what `kmain` has become:

```c
for (;;)
    __asm__ volatile("wfi");    // sleep until an interrupt
```

`wfi` — *wait for interrupt* — stops the core in a low-power state. The kernel now literally idles at ~0% CPU and wakes exactly when there's work. Every OS you've ever used spends most of its life in a loop shaped exactly like this one.

## 2. The interrupt delivery chain

Between "timer expires" and "our handler runs" sits a chain, and **every link must be switched on**:

```
timer ──> GIC distributor ──> GIC CPU interface ──> CPU IRQ line ──> vector entry 5
          (global routing)     (per-core delivery)    (DAIF mask)
```

The **GIC** (Generic Interrupt Controller) is ARM's standard interrupt hub, here a GICv2 — we read its addresses out of QEMU's own device tree rather than trusting a tutorial: distributor at `0x0800_0000`, CPU interface at `0x0801_0000`. The **distributor** decides globally which interrupts are live and where they route; the **CPU interface** is each core's personal mailbox with priority filtering.

The five switches, in the order the code flips them:

| Switch | Where | Code |
|---|---|---|
| 1. Distributor on | GICD_CTLR | `GICD_CTLR = 1` |
| 2. This interrupt enabled | GICD_ISENABLER | `gic_enable_intid(30)` |
| 3. Priority mask open | GICC_PMR | `GICC_PMR = 0xff` |
| 4. CPU interface on | GICC_CTLR | `GICC_CTLR = 1` |
| 5. CPU accepting IRQs | DAIF register | `msr daifclr, #2` |

Memorize this as a checklist. "My interrupt doesn't fire" is *the* classic osdev symptom, and the cause is always one of these five left off. (A sixth for timers: the device itself enabled — `CNTP_CTL_EL0 = 1`.)

One nice detail in `gic_enable_intid`: the ISENABLER registers are *write-1-to-set* — writing 0 bits does nothing — so enabling one interrupt needs no read-modify-write and can't race.

## 3. The INTID space

Every interrupt has a number, and the number tells you what kind it is:

- **0–15: SGIs** (software-generated) — cores poking each other; matters for multicore, not us yet.
- **16–31: PPIs** (private peripheral) — per-core devices. Each core has its *own* timer, so the timer is PPI 14 → INTID 16+14 = **30**.
- **32+: SPIs** (shared peripheral) — normal devices like the UART (INTID 33 on virt).
- **1020–1023: special.** 1023 means *spurious* — "something was pending when you asked, but it's gone." You ack'd it, but you must **not** EOI it. Our handler checks `intid >= 1020` before anything else.

## 4. The ack/EOI protocol

Talking to the GIC per interrupt is a strict two-step:

```c
uint32_t intid = gic_ack();     // read IAR: "I'm handling this one"
/* ... handle it ... */
gic_eoi(intid);                 // write EOIR: "done with this one"
```

Reading `GICC_IAR` *claims* the interrupt — the GIC marks it active and won't re-deliver it. Writing `GICC_EOIR` releases it. Between the two, that interrupt cannot recurse into you.

Two bugs with identical symptoms — "tick 1" then eternal silence:
- **Forget the EOI**: the GIC considers INTID 30 still active and politely never delivers it again.
- **Forget to re-arm TVAL**: the GIC is fine, but the timer never asks again.

Same symptom, different subsystem. (Section 7 makes you build both.)

## 5. The generic timer

Unlike the GIC, the timer isn't MMIO at all — it lives in **system registers**, accessed with `mrs`/`msr` like `ESR_EL1` was:

- **`CNTFRQ_EL0`** — how fast the global counter runs. QEMU virt: 62,500,000 Hz (the boot line prints it). Real hardware differs — 24 MHz is common — which is why the code *reads* the frequency instead of hardcoding it.
- **`CNTP_TVAL_EL0`** — a countdown: "fire when this many counter ticks have elapsed." Writing `62500000` at 62.5 MHz means "in one second."
- **`CNTP_CTL_EL0`** — bit 0 enables the timer.

TVAL is one-shot by design: it fires once and stays fired. Periodic behavior is our job — the handler's last act is `write_cntp_tval(ticks_per_period)`, resetting the countdown:

```c
void timer_tick(void)
{
    tick_count++;
    kprintf("tick %d\n", (int)tick_count);
    write_cntp_tval(ticks_per_period);  // re-arm: TVAL counts down anew
}
```

## 6. eret: the road back

Milestone 3's exception path ended in `panic` — a one-way trip. The IRQ path is round-trip, and the return half is the new machinery:

```asm
irq_stub:
    save_rest                       // same trap frame as milestone 3
    mov     x0, sp
    bl      handle_irq
    ldp     x1, x2, [sp, #248]      // elr, spsr (adjacent: one ldp)
    msr     elr_el1, x1
    msr     spsr_el1, x2
    ldr     x30, [sp, #240]
    ...                             // every GPR, in reverse
    ldp     x0, x1, [sp, #0]        // scratch registers last
    add     sp, sp, #288
    eret
```

Order matters twice. ELR/SPSR are restored *first* because `msr` needs scratch registers (x1, x2) that themselves still need restoring afterward. And x0/x1 come *dead last* because they were saved first and everything else has been reloaded by then. The final `eret` is the magic: it atomically restores the PC from `ELR_EL1` and processor state from `SPSR_EL1`. The interrupted `wfi` loop resumes with every register bit-identical — it cannot tell it was ever interrupted.

Here's the punchline for where we're headed: **this save/restore discipline is ~80% of a context switch**. Milestone 6's scheduler will do exactly this — except between `bl handle_irq` and the restore, it may swap which stack pointer (and therefore *whose* saved registers) the restore reads from. Different frame in, different code resumes. That's all "switching processes" is.

## 7. Try breaking it

1. **Comment out `gic_eoi(intid)`** in `handle_irq`. Tick 1, then silence — the GIC never re-delivers an interrupt you didn't finish.
2. **Comment out the re-arm** (`write_cntp_tval` in `timer_tick`) instead. Same silence, different culprit. How would you tell them apart at 3am? Read `CNTP_CTL_EL0`: bit 2 (ISTATUS) is 1 when the timer condition is met — set means "timer fired but delivery is stuck" (EOI bug), clear means "timer never fired" (re-arm bug).
3. **`timer_init(10)`** — ten ticks per second. One number, and you've chosen your kernel's time-slice granularity; remember this knob when the scheduler starts slicing.

## Where we go next

Milestone 5: memory. First a physical page allocator (so the kernel can hand out RAM), then the MMU — page tables, virtual addresses, and the moment the unaligned-read crash from milestone 3 stops crashing because RAM becomes Normal memory. After that, processes have somewhere private to live.
