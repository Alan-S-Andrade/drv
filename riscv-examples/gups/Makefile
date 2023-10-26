DRV_DIR := $(shell git rev-parse --show-toplevel)
TARGET := exe.riscv
PLATFORM ?= ph

CFLAGS += -DDRAM_BASE=0x40000000 -DDRAM_SIZE=0x40000000
CFLAGS += -DTHREAD_UPDATES=1024 -DTABLE_SIZE=$(shell echo 64*1024*1024 | bc)

include $(DRV_DIR)/mk/riscv_common.mk
