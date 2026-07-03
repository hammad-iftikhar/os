#include "kprintf.h"
#include "trap.h"
#include "gic.h"
#include "timer.h"
#include "pmm.h"

void kmain(void)
{
    exceptions_init();
    gic_init();
    pmm_init();

    // Allocator smoke test: LIFO free list means freeing then
    // reallocating returns the same page.
    void *a = alloc_page();
    void *b = alloc_page();
    kprintf("pmm: a=%p b=%p\n", a, b);
    free_page(a);
    void *c = alloc_page();
    kprintf("pmm: freed a, realloc'd -> %p (%s)\n",
            c, c == a ? "same page, LIFO works" : "BUG: expected a");

    timer_init(1);
    __asm__ volatile("msr daifclr, #2");
    kprintf("interrupts on; idling\n");

    for (;;)
        __asm__ volatile("wfi");
}
