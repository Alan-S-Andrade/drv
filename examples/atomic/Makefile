# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR := $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk
APP_NAME := atomic
include $(DRV_DIR)/mk/application_common.mk

N      ?= 1000
TARGET ?= DRAM
MEMOP  ?= LOAD

CXXFLAGS += -DNATOMICS=$(N) -DTARGET=TAGET_$(TARGET) -DMEMOP_$(MEMOP)
SIM_OPTIONS += --core-threads=1 --pod-cores=1 --num-pxn=1

MEM_BACKENDS-yes += simple
#MEM_BACKENDS-$(RAMULATOR) += ramulator
MEM_BACKENDS := $(MEM_BACKENDS-yes)

tests := $(foreach membackend,$(MEM_BACKENDS),run-$(membackend))

ramulator-SIM_OPTIONS := --dram-backend-config $(RAMULATOR_TEST_CONFIG)

run-%: $(APP_NAME).so
	$(eval SIM_OPTIONS=$($*-SIM_OPTIONS))
	$(SST) $(DRV_DIR)/tests/$(SCRIPT) -- --dram-backend $* $(all-SIM_OPTIONS) $(SIM_OPTIONS) $(APP_PATH)/$(APP_NAME).so

run: $(tests)
	@echo Running tests
