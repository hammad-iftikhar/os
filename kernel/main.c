#include "kprintf.h"
#include "trap.h"
#include "gic.h"
#include "timer.h"

void kmain(void)
{
    exceptions_init();
    gic_init();
    timer_init(1);                          // 1 Hz heartbeat

    __asm__ volatile("msr daifclr, #2");    // unmask IRQs (the I in DAIF)
    kprintf("interrupts on; idling\n");

    for (;;)
        __asm__ volatile("wfi");            // sleep until an interrupt
}
