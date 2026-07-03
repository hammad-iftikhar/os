#pragma once

#include <stdint.h>

// Filled in by the vector stubs in vectors.S. Field order and offsets
// are a contract with that code -- do not reorder.
struct trap_frame {
    uint64_t x[31];     // x0..x30
    uint64_t elr;       // where the exception happened
    uint64_t spsr;      // saved processor state
    uint64_t esr;       // exception syndrome: what and why
    uint64_t far;       // faulting address (valid for aborts)
};

void exceptions_init(void);
