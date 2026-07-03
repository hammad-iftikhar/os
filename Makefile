CROSS  := --target=aarch64-elf
CC     := clang
CFLAGS := $(CROSS) -ffreestanding -mgeneral-regs-only -Wall -Wextra -O2 -g
LD     := ld.lld
QEMU   := qemu-system-aarch64 -machine virt -cpu cortex-a72

OBJS := build/boot.o build/main.o build/uart.o build/kprintf.o \
        build/vectors.o build/trap.o build/gic.o build/timer.o

all: build/kernel.elf

build:
	mkdir -p build

build/boot.o: boot.S | build
	$(CC) $(CROSS) -c $< -o $@

build/%.o: kernel/%.S | build
	$(CC) $(CROSS) -c $< -o $@

build/%.o: kernel/%.c kernel/uart.h kernel/kprintf.h kernel/trap.h \
           kernel/gic.h kernel/timer.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/kernel.elf: $(OBJS) linker.ld
	$(LD) -T linker.ld $(OBJS) -o $@

run: build/kernel.elf
	$(QEMU) -nographic -kernel $<

clean:
	rm -rf build

.PHONY: all run clean
