# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR  ?= $(shell git rev-parse --show-toplevel)
SCRIPT   := $(DRV_DIR)/tests/PANDOHammerDrvR.py

RISCV_COMPILE_FLAGS += -nostartfiles
RISCV_COMPILE_FLAGS += -I$(DRV_DIR)/riscv-examples/platform_ph
RISCV_CFLAGS   += $(RISCV_COMPILE_FLAGS)
RISCV_CXXFLAGS += $(RISCV_COMPILE_FLAGS)

RISCV_LDFLAGS += -Wl,-T$(DRV_DIR)/riscv-examples/platform_ph/bsg_link.ld
RISCV_LDFLAGS += -L$(DRV_DIR)/riscv-examples/platform_ph/pandohammer

# include platform crt by default
RISCV_PLATFORM_CRT ?= yes
RISCV_PLATFORM_LIBC_LOCKING ?= yes

# platform asm sources
RISCV_PLATFORM_ASMSOURCE-$(RISCV_PLATFORM_CRT) += crt.S
RISCV_PLATFORM_CSOURCE-$(RISCV_PLATFORM_LIBC_LOCKING) += lock.c

# add platform crt to asmsource
RISCV_ASMSOURCE += $(RISCV_PLATFORM_ASMSOURCE-yes)
RISCV_CSOURCE   += $(RISCV_PLATFORM_CSOURCE-yes)
