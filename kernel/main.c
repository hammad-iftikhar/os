#include "kprintf.h"
#include "trap.h"
#include "gic.h"
#include "timer.h"
#include "pmm.h"
#include "vm.h"
#include "thread.h"
#include "user.h"

// One kernel thread keeps running alongside the user program to show
// EL0 code and EL1 threads share the same scheduler heartbeat.
static void worker_k(void)
{
    for (int n = 1;; n++) {
        kprintf("  K (kernel thread) says %d\n", n);
        for (volatile int i = 0; i < 40000000; i++)
            ;
    }
}

void kmain(void)
{
    exceptions_init();
    gic_init();
    pmm_init();
    vm_init();
    thread_bootstrap();

    thread_create(worker_k, "K");

    timer_init(10);                         // 10 Hz preemption
    __asm__ volatile("msr daifclr, #2");

    // The main thread itself becomes the user program's vehicle:
    // every trap from EL0 lands on this thread's kernel stack.
    user_run();
}
