#include "kprintf.h"
#include "trap.h"
#include "gic.h"
#include "timer.h"
#include "pmm.h"
#include "vm.h"
#include "thread.h"

// Two workers that never yield voluntarily: only preemption can
// interleave them. The busy loop is the point -- it proves the timer
// can yank the CPU away from uncooperative code.
static void spin(void)
{
    for (volatile int i = 0; i < 20000000; i++)
        ;
}

static void worker_a(void)
{
    for (int n = 1;; n++) {
        kprintf("  A says %d\n", n);
        spin();
    }
}

static void worker_b(void)
{
    for (int n = 1;; n++) {
        kprintf("  B says %d\n", n);
        spin();
    }
}

void kmain(void)
{
    exceptions_init();
    gic_init();
    pmm_init();
    vm_init();
    thread_bootstrap();

    thread_create(worker_a, "A");
    thread_create(worker_b, "B");

    timer_init(10);                         // 10 Hz preemption
    __asm__ volatile("msr daifclr, #2");
    kprintf("scheduler on; main thread idling\n");

    for (;;)
        __asm__ volatile("wfi");
}
