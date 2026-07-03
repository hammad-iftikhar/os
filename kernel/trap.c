#include <stdint.h>
#include "kprintf.h"
#include "trap.h"

// Filled in by save_rest in vectors.S. Field order and offsets are a
// contract with that code -- do not reorder.
struct trap_frame {
    uint64_t x[31];     // x0..x30
    uint64_t elr;       // where the exception happened
    uint64_t spsr;      // saved processor state
    uint64_t esr;       // exception syndrome: what and why
    uint64_t far;       // faulting address (valid for aborts)
};

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

void exceptions_init(void)
{
    extern char vectors[];

    __asm__ volatile("msr vbar_el1, %0" :: "r"(vectors));
}
