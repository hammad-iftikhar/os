#include <stdint.h>
#include "kprintf.h"
#include "trap.h"
#include "gic.h"
#include "timer.h"
#include "thread.h"
#include "syscall.h"

static const char *vector_names[16] = {
    "sync EL1t", "irq EL1t", "fiq EL1t", "serror EL1t",
    "sync EL1h", "irq EL1h", "fiq EL1h", "serror EL1h",
    "sync EL0/64", "irq EL0/64", "fiq EL0/64", "serror EL0/64",
    "sync EL0/32", "irq EL0/32", "fiq EL0/32", "serror EL0/32",
};

// ESR_EL1[31:26] = exception class. The few we can hit now.
static const char *ec_name(uint32_t ec)
{
    switch (ec) {
    case 0x15: return "SVC instruction";
    case 0x20: return "instruction abort, lower EL";
    case 0x21: return "instruction abort, same EL";
    case 0x22: return "PC alignment fault";
    case 0x24: return "data abort, lower EL";
    case 0x25: return "data abort, same EL";
    case 0x26: return "SP alignment fault";
    case 0x3c: return "BRK instruction";
    default:   return "unknown";
    }
}

void handle_exception(struct trap_frame *tf, uint64_t vector);

void handle_exception(struct trap_frame *tf, uint64_t vector)
{
    uint32_t ec = (uint32_t)(tf->esr >> 26) & 0x3f;

    kprintf("\n*** EXCEPTION: %s ***\n", vector_names[vector & 0xf]);
    kprintf("ESR  = %p  EC=%x: %s\n", (void *)tf->esr, ec, ec_name(ec));
    kprintf("ELR  = %p  (faulting instruction)\n", (void *)tf->elr);
    kprintf("FAR  = %p  (faulting address)\n", (void *)tf->far);
    kprintf("SPSR = %p\n", (void *)tf->spsr);

    for (int i = 0; i < 31; i += 2) {
        kprintf("x%d%s = %p", i, i < 10 ? " " : "", (void *)tf->x[i]);
        if (i + 1 < 31)
            kprintf("   x%d%s = %p", i + 1, i + 1 < 10 ? " " : "",
                    (void *)tf->x[i + 1]);
        kprintf("\n");
    }

    panic("unhandled exception");
}

void handle_irq(struct trap_frame *tf);

void handle_irq(struct trap_frame *tf)
{
    (void)tf;   // milestone 6's scheduler will use the frame

    uint32_t intid = gic_ack();

    if (intid >= 1020)              // spurious: ack'd by someone else
        return;                     // (no EOI for spurious IDs)

    if (intid != TIMER_INTID)
        panic("unexpected IRQ %d", (int)intid);

    // Order is load-bearing: re-arm and EOI *before* schedule().
    // schedule() may not return for many ticks (we'll be switched
    // out); if the EOI waited until after, the GIC would consider
    // the timer interrupt still active and never deliver another
    // one -- preemption would end at the first switch.
    timer_tick();
    gic_eoi(intid);
    schedule();
}

void handle_sync_el0(struct trap_frame *tf);

void handle_sync_el0(struct trap_frame *tf)
{
    uint32_t ec = (uint32_t)(tf->esr >> 26) & 0x3f;

    if (ec == 0x15) {                   // SVC: a syscall
        // ELR already points past the svc instruction; returning
        // resumes the user program with the result in its x0.
        tf->x[0] = syscall_dispatch(tf);
        return;
    }

    // Any other sync exception from EL0 is a user program fault.
    handle_exception(tf, 8);            // dumps registers and panics
}

void exceptions_init(void)
{
    extern char vectors[];

    __asm__ volatile("msr vbar_el1, %0" :: "r"(vectors));
}
