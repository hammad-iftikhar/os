#pragma once

#include <stdint.h>
#include "trap.h"

#define SYS_write   0
#define SYS_ticks   1

uint64_t syscall_dispatch(struct trap_frame *tf);
