# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR  ?= $(shell git rev-parse --show-toplevel)
SCRIPT   := $(DRV_DIR)/tests/riscvhb.py
CFLAGS   += -nostartfiles
CXXFLAGS += -nostartfiles
LDFLAGS  += -Wl,-T$(DRV_DIR)/riscv-examples/platform_hb/bsg_link.ld

$(TARGET): crt.o

# $(TARGET): bsg_link.ld

# bsg_link.ld: $(DRV_DIR)/py/bsg_manycore_link_gen.py
# 	$(PYTHON) $< > $@


