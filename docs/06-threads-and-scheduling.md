# Milestone 6: Threads & Scheduling

Two loops that never yield, never sleep, never cooperate — and yet they take turns. Every interleave in the output is the kernel forcibly confiscating the CPU from one uncooperative busy-loop and handing it to another. This is preemptive multitasking, and it took surprisingly little new code: one 40-line assembly function and one array.

## 1. What a thread actually is

Strip away the mystique: a thread is **a stack plus saved registers**. Ours is literally:

```c
struct thread {
    struct context ctx;     // 13 saved registers, 104 bytes
    enum thread_state state;
    const char *name;
};
```

...plus one 4 KiB page for its stack. That's the entire ontology. "Multitasking" just means the CPU's registers are a shared resource that threads take turns wearing. When thread A isn't running, "A" exists only as 104 bytes of parked registers and whatever its stack remembers.

## 2. Why swtch saves only x19–x30 + sp

The whole context switch:

```asm
swtch:
    stp     x19, x20, [x0, #0]      // save callee-saved regs into old...
    ...
    stp     x29, x30, [x0, #80]
    mov     x9, sp
    str     x9, [x0, #96]

    ldp     x19, x20, [x1, #0]      // ...load them from new...
    ...
    ldr     x9, [x1, #96]
    mov     sp, x9
    ret                             // ...and return as the new thread
```

Thirteen registers. Where are the other eighteen? The answer is the AAPCS64 calling convention. `swtch` is an *ordinary function call*, and the ABI splits registers into two classes: **caller-saved** (x0–x18: a called function may trash them, so the caller already spilled anything it needed) and **callee-saved** (x19–x28, x29, x30: a function must preserve them). So by the time we're inside `swtch`, x0–x18 are *already dead or already saved* by compiler-generated code. Callee-saved registers are literally defined as "state that survives a function call" — which makes them exactly and only the state that must survive a context switch.

"But a preempted thread was interrupted mid-`spin()`, with live values in x0–x18!" — those are already safe too: the moment the timer fired, `irq_stub` saved **all 31 registers** into a trap frame *on that thread's own stack* (milestone 3's machinery). By the time `schedule()` calls `swtch`, the thread's complete state is: trap frame on its stack + callee-saved regs of the handler path. `swtch` switches stacks; the rest follows.

The last two lines are the sleight of hand. After `mov sp, x9`, we're standing on the *new* thread's stack; `ret` jumps to the *new* thread's x30. Same function, entered as A, returned as B.

## 3. Anatomy of a preemptive switch

Follow one tick, A → B, both mid-busy-loop:

```
A: spin()                            B (parked): stack holds
   │ timer fires                        [trap frame from B's last tick]
   ▼                                    [handler frames]
   irq_stub: A's 31 regs → trap         [schedule() frame]
             frame on A's stack         ctx: callee-saved + sp
   ▼
   handle_irq → timer_tick, gic_eoi
   ▼
   schedule(): picks B
   ▼
   swtch(&A.ctx, &B.ctx)   ←──── the moment. sp now = B's stack
   ▼
   returns INTO B's schedule() call  (B parked exactly here, ticks ago)
   ▼
   B unwinds: schedule → handle_irq → irq_stub restore → eret
   ▼
B: spin() continues at the instruction the timer interrupted
```

Perfect symmetry: A is now parked in exactly the shape B was. The trap-frame save/restore from milestone 4 didn't change at all — what changed is *whose* frame the unwinding restore reads, because `swtch` changed the stack under it. That's the one new trick, as promised.

## 4. Birth of a thread

A brand-new thread has never called `swtch`, so `thread_create` forges the context a switch *would* have left behind:

```c
t->ctx.x19 = (uint64_t)fn;      // thread_start jumps via x19
t->ctx.x30 = (uint64_t)thread_start;
t->ctx.sp = (uint64_t)stack + PAGE_SIZE;    // stacks grow down
```

When `swtch` loads this and `ret`s, it "returns" to `thread_start` — an address no call ever came from:

```asm
thread_start:
    msr     daifclr, #2     // we arrived via the IRQ path: unmask
    blr     x19             // run the thread function
    bl      thread_return_panic
```

Two duties: unmask IRQs (we arrived through the masked timer-IRQ path; a veteran thread gets unmasked by its `eret`, a newborn has no `eret` pending — forget this and the first-created thread runs forever, unpreemptable), then jump to the function. A forged return address redirecting `ret` somewhere no call came from is, pointed the other way, exactly how ROP attacks work; here it's how threads are born.

## 5. The EOI-before-schedule bug

The signature trap of this milestone lives in three lines of `handle_irq`:

```c
timer_tick();       // re-arm the countdown
gic_eoi(intid);     // tell the GIC we're done
schedule();         // may not return for many ticks!
```

Suppose you wrote the "natural" order — handle, schedule, then EOI on the way out. `schedule()` switches to B, and A — with its un-EOI'd interrupt — won't resume for ticks. Meanwhile the GIC still considers INTID 30 **active**, and the GIC's rule is: an active interrupt is not re-delivered. So no more timer interrupts arrive. B keeps the CPU forever. The cruelest part is the symptom: "B says 1, 2, 3... forever, A never speaks again" — which looks *exactly* like a scheduler bug. You'd stare at `schedule()` all night, and `schedule()` is perfect. In interrupt handlers, ordering is architecture.

## 6. Atomic kprintf: the first critical section

Preemption's first casualty is shared state, and our first shared state is the UART. A thread preempted halfway through printing `"  A says 12\n"` leaves the line half-written; B prints into the middle of it; the output garbles. The fix in `kprintf`:

```c
__asm__ volatile("mrs %0, daif" : "=r"(daif));   // save interrupt state
__asm__ volatile("msr daifset, #2");             // mask
/* ... print ... */
__asm__ volatile("msr daif, %0" :: "r"(daif));   // restore
```

Note it's save/restore, not blind mask/unmask: `kprintf` is also called from *inside* the IRQ handler, where interrupts must stay masked after we're done. Blindly clearing would re-enable interrupts in a context that assumes they're off. This save-mask-restore pattern is the project's first **critical section** — and the seed of every lock this kernel will ever grow.

## 7. What the demo proves

```
  A says 1
  A says 2
  B says 1
  B says 2
  A says 3
```

Neither worker contains a single cooperative instruction — no yield, no sleep, just `kprintf` and a 20-million-iteration busy `spin()`. The runs-of-two pattern is legible physics: at 10 Hz preemption, each thread gets 100 ms per slice, and one print+spin apparently takes ~50 ms under QEMU, so two iterations fit per slice. Change `spin()`'s constant or the timer's Hz and the run length shifts accordingly — the output is a measurement, not an accident.

(Why doesn't the `main` thread starve the workers? It does get scheduled — but it immediately hits `wfi` and donates the rest of its slice to the next tick. Accidental politeness, not design. A real idle thread would `yield`.)

## 8. Try breaking it

1. **Swap the order**: move `gic_eoi(intid)` after `schedule()` in `handle_irq`. Watch one worker monopolize the CPU forever — the signature bug, live. Now you've *seen* the symptom-that-looks-like-a-scheduler-bug; you'll recognize it in a real codebase someday.
2. **Un-atomic kprintf**: remove the DAIF save/mask/restore. Run a while; hunt for a line where A's and B's characters interleave. Notice how *rare* it is (prints are short; ticks every 100 ms) — this is why unlocked-shared-state bugs ship to production.
3. **Add worker C** and predict, before booting, the exact rotation. Round-robin over the table is deterministic: A → B → C → main → A. Check yourself against the output.

## Where we go next

Milestone 7, the finale of the original roadmap: user mode. Threads drop to EL0 where the hardware forbids them from touching kernel memory or devices; the only way back in is `svc` — the syscall. The vector-table rows for "lower EL" finally earn their keep, and this stops being a program that juggles functions and becomes an operating system that runs *programs*.
