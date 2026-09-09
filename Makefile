# Makefile for Cact resolved (kernel-message logger daemon)

ROOT := $(abspath .)

# CactLibc project dir (contains build/pic/start.o and clibc.so)
CACTLIB ?= $(abspath ../CactLibc-x86_32)
# Staging dir of LocalRepoCactOS where /sbin apps are collected
LR_SBIN ?= $(abspath ../LocalRepoCactOS-x86_32/lib/sbin)

CC      := gcc
LD      := ld
START_O := $(CACTLIB)/build/pic/start.o
LIBC_SO := $(CACTLIB)/clibc.so

CFLAGS := -m32 -ffreestanding -fPIE -fno-stack-protector -nostdlib \
          -ffunction-sections -fdata-sections \
          -I$(CACTLIB)/include -Wall -Wextra

LDFLAGS := -m elf_i386 -pie --dynamic-linker=/lib/ld.so --hash-style=both \
           -nostdlib --gc-sections -T $(ROOT)/link.ld

OUT  := build/sbin/resolved

.PHONY: all install clean

all: $(OUT)

$(START_O) $(LIBC_SO):
	@test -f $(LIBC_SO) && test -f $(START_O) || (echo >&2 "Missing libc — build CactLibc first (CACTLIB=$(CACTLIB))"; exit 1)

build/resolved.o: main.c $(START_O) $(LIBC_SO)
	@mkdir -p build
	$(CC) $(CFLAGS) -c main.c -o $@

$(OUT): build/resolved.o $(START_O) $(LIBC_SO)
	@mkdir -p build/sbin
	$(LD) $(LDFLAGS) $(START_O) build/resolved.o $(LIBC_SO) -o $@

install: all
	@mkdir -p $(LR_SBIN)
	cp -f $(OUT) $(LR_SBIN)/resolved

clean:
	rm -rf build
