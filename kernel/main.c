#include "kprintf.h"
#include "trap.h"
#include "gic.h"
#include "timer.h"
#include "pmm.h"
#include "vm.h"

void kmain(void)
{
    exceptions_init();
    gic_init();
    pmm_init();
    vm_init();

    // Milestone 3 payoff: this exact read used to be a data abort.
    // RAM is Normal memory now -- unaligned access is legal.
    volatile unsigned int *p = (unsigned int *)0x40080001UL;
    kprintf("unaligned read at %p works now: %x\n", (void *)p, *p);

    timer_init(1);
    __asm__ volatile("msr daifclr, #2");
    kprintf("interrupts on; idling\n");

    for (;;)
        __asm__ volatile("wfi");
}
