# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR := $(shell git rev-parse --show-toplevel)

TARGET := exe.riscv
PLATFORM ?= ph

SIM_OPTIONS += --verbose 0
# SIM_OPTIONS += --debug-memory
# SIM_OPTIONS += --debug-responses
# SIM_OPTIONS += --debug-requests
SIM_OPTIONS += --verbose-memory 0
SIM_OPTIONS += --pod-cores=1 --core-threads=1

include $(DRV_DIR)/mk/riscv_common.mk
