# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR  ?= $(shell git rev-parse --show-toplevel)
SCRIPT   := $(DRV_DIR)/tests/PANDOHammerDrvR.py

COMPILE_FLAGS += -nostartfiles
COMPILE_FLAGS += -I$(DRV_DIR)/riscv-examples/platform_ph
CFLAGS   += $(COMPILE_FLAGS)
CXXFLAGS += $(COMPILE_FLAGS)

LDFLAGS += -Wl,-T$(DRV_DIR)/riscv-examples/platform_ph/bsg_link.ld
LDFLAGS += -L$(DRV_DIR)/riscv-examples/platform_ph/pandohammer

exe.riscv: crt.o
