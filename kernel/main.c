#include "kprintf.h"
#include "trap.h"

void kmain(void)
{
    exceptions_init();
    kprintf("Hello from kernel\n");
    kprintf("vectors installed; now crashing on purpose...\n");

    // With the MMU off, all memory is Device-type and unaligned
    // access faults: data abort, FAR = the bad address.
    volatile unsigned int *p = (unsigned int *)0x40080001UL;
    kprintf("read: %x\n", *p);

    panic("unreachable: the read above must fault");
}
