#pragma once

#include <stdint.h>

// EL1 physical timer: private peripheral interrupt 14 -> INTID 16+14.
#define TIMER_INTID 30

void timer_init(uint32_t hz);
void timer_tick(void);
