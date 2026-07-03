#include <stdint.h>
#include "syscall.h"
#include "kprintf.h"
#include "timer.h"
#include "uart.h"
#include "vm.h"

// The trust boundary. Everything arriving from EL0 is hostile until
// proven otherwise -- a user pointer is just an integer someone at
// EL0 chose. Validate against the user's own address range before
// the kernel dereferences anything.

static uint64_t sys_write(uint64_t uptr)
{
    if (uptr < USER_CODE_VA || uptr >= USER_STACK_TOP) {
        kprintf("syscall: write with bad pointer %p, rejected\n",
                (void *)uptr);
        return (uint64_t)-1;
    }

    // Bounded walk: stop at NUL, the end of user memory, or 256 bytes.
    const char *s = (const char *)uptr;
    for (int i = 0; i < 256 && (uint64_t)&s[i] < USER_STACK_TOP; i++) {
        if (!s[i])
            return i;
        if (s[i] == '\n')
            uart_putc('\r');
        uart_putc(s[i]);
    }
    return (uint64_t)-1;
}

uint64_t syscall_dispatch(struct trap_frame *tf)
{
    switch (tf->x[8]) {
    case SYS_write:
        return sys_write(tf->x[0]);
    case SYS_ticks:
        return timer_ticks();
    default:
        // A user bug is not a kernel bug: refuse, don't panic.
        kprintf("syscall: unknown number %d\n", (int)tf->x[8]);
        return (uint64_t)-1;
    }
}
