# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR := $(shell git rev-parse --show-toplevel)
RISCV_CSOURCE := $(wildcard *.c)
RISCV_COMPILE_FLAGS += -DDRAM_BASE=0x40000000 -DDRAM_SIZE=0x40000000
RISCV_COMPILE_FLAGS += -DTHREAD_UPDATES=1024 -DTABLE_SIZE=$(shell echo 1*1024*1024 | bc)

include $(DRV_DIR)/mk/riscv_common.mk
