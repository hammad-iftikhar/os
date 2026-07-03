#pragma once

#include <stdint.h>

void gic_init(void);
void gic_enable_intid(uint32_t intid);
uint32_t gic_ack(void);             // returns INTID (>=1020 = spurious)
void gic_eoi(uint32_t intid);
