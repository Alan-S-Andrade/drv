# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR ?= $(shell git rev-parse --show-toplevel)
SCRIPT := $(DRV_DIR)/tests/riscvcrt.py
