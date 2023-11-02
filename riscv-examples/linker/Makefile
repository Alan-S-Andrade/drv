# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR := $(shell git rev-parse --show-toplevel)
TARGET := exe.riscv
PLATFORM ?= ph
SIM_OPTIONS += --pod-cores=1 --core-threads=1
COMPILE_FLAGS += -Wl,--defsym=z=0
include $(DRV_DIR)/mk/riscv_common.mk
