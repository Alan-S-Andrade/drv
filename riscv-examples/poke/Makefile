# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR := $(shell git rev-parse --show-toplevel)
TARGET := exe.riscv
PLATFORM ?= ph
SIM_OPTIONS += --verbose 100 --debug-memory

COMPILE_FLAGS += -nostartfiles

STORE_ADDR ?= 0x000000004100101f
COMPILE_FLAGS += -DSTORE_ADDR=$(STORE_ADDR)
COMPILE_FLAGS += -DSTORE_VALUE=0xfe


include $(DRV_DIR)/mk/riscv_common.mk

$(TARGET): crt.o
