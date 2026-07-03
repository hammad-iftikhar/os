#pragma once

#include <stdarg.h>

void kprintf(const char *fmt, ...);
void vkprintf(const char *fmt, va_list ap);
_Noreturn void panic(const char *fmt, ...);
