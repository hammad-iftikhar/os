#pragma once

#include <stdint.h>

// User address space: L1 slot 2, far from the identity-mapped kernel
// (slot 0 = devices, slot 1 = RAM). One code page, one stack page.
#define USER_CODE_VA    0x80000000UL
#define USER_STACK_VA   0x80001000UL
#define USER_STACK_TOP  (USER_STACK_VA + 0x1000)

void vm_init(void);
void vm_map_user(uint64_t va, uint64_t pa, int exec);   // exec: RX else RW
