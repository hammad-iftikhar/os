#pragma once

#include <stdint.h>

// Callee-saved state, the only registers that survive a function call.
// Field offsets are a contract with swtch.S -- do not reorder.
struct context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t x29;   // frame pointer
    uint64_t x30;   // link register: where swtch returns to
    uint64_t sp;
};

enum thread_state {
    THREAD_UNUSED = 0,
    THREAD_RUNNABLE,
    THREAD_RUNNING,
};

struct thread {
    struct context ctx;
    enum thread_state state;
    const char *name;
};

void thread_bootstrap(void);    // adopt kmain as thread 0
void thread_create(void (*fn)(void), const char *name);
void schedule(void);            // round-robin; call with IRQs masked
