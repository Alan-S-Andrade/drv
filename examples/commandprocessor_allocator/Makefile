# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR := $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk
APP_NAME := commandprocessor
APP_PATH := $(DRV_DIR)/examples/commandprocessor_allocator

include $(DRV_DIR)/mk/application_common.mk

run: $(APP_NAME).so
	sst $(DRV_DIR)/tests/$(SCRIPT) -- \
	--with-command-processor=$(APP_PATH)/$(APP_NAME).so \
	--pod-cores=1 \
	--core-threads=1 \
	$(APP_PATH)/$(APP_NAME).so
