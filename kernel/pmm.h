#pragma once

#include <stdint.h>

#define PAGE_SIZE 4096UL

void pmm_init(void);
void *alloc_page(void);         // 4 KiB, zeroed; panics when out
void free_page(void *p);
uint64_t pmm_free_count(void);
