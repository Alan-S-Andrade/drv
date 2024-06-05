# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR := $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk
APP_NAME := atomic_read_back
include $(DRV_DIR)/mk/application_common.mk


TARGET ?= L1SP
DRAM_ACCESS_TIME ?= 1000ns
MAX_IDLE ?= 1

THREADS ?= 1
CORES   ?= 1
PODS    ?= 1
PXNS    ?= 1

CXXFLAGS += -DTARGET_$(TARGET) 
SIM_OPTIONS += --core-threads=$(THREADS) --pod-cores=$(CORES) --pxn-pods=$(PODS) --num-pxn=$(PXNS)
SIM_OPTIONS += --dram-access-time=$(DRAM_ACCESS_TIME)
SIM_OPTIONS += --core-max-idle=$(MAX_IDLE)

MEM_BACKENDS-yes += simple
#MEM_BACKENDS-$(RAMULATOR) += ramulator
MEM_BACKENDS := $(MEM_BACKENDS-yes)

tests := $(foreach membackend,$(MEM_BACKENDS),run-$(membackend))

ramulator-SIM_OPTIONS := --dram-backend-config $(RAMULATOR_TEST_CONFIG)

SCRIPT := drv_test.py

run-%: $(APP_NAME).so
	$(eval SIM_OPTIONS=$($*-SIM_OPTIONS) $(SIM_OPTIONS))
	@echo N=$(N)
	@echo TARGET=$(TARGET)
	@echo MEMOP=$(LOAD)
	@echo DRAM_ACCESS_TIME=$(DRAM_ACCESS_TIME)
	@echo MAX_IDLE=$(MAX_IDLE)
	$(SST) -n $(SIM_THREADS) $(DRV_DIR)/tests/$(SCRIPT) -- --dram-backend $* $(all-SIM_OPTIONS) $(SIM_OPTIONS) $(APP_PATH)/$(APP_NAME).so

run: $(tests)
	@echo Running tests
