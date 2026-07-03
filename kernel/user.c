#include <stdint.h>
#include "user.h"
#include "pmm.h"
#include "vm.h"
#include "string.h"
#include "kprintf.h"

extern char __user_code_start[], __user_code_end[];
extern void enter_user(uint64_t entry, uint64_t sp);

void user_run(void)
{
    void *code = alloc_page();
    void *stack = alloc_page();

    // The blob is position-independent; copy it out of the kernel
    // image into the user's own page.
    memcpy(code, __user_code_start,
           (unsigned long)(__user_code_end - __user_code_start));

    // We wrote instructions through the data side; make sure the
    // instruction side can't see stale bytes.
    __asm__ volatile("dc cvau, %0\n dsb ish\n ic iallu\n dsb ish\n isb"
                     :: "r"(code));

    vm_map_user(USER_CODE_VA, (uint64_t)code, 1);
    vm_map_user(USER_STACK_VA, (uint64_t)stack, 0);

    kprintf("user: %d bytes of code at VA %p, dropping to EL0\n",
            (int)(__user_code_end - __user_code_start),
            (void *)USER_CODE_VA);

    enter_user(USER_CODE_VA, USER_STACK_TOP);
}
