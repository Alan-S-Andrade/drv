# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR := $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk
APP_NAME := cas
include $(DRV_DIR)/mk/application_common.mk

run: $(APP_NAME).so
	sst $(DRV_DIR)/tests/$(SCRIPT) -- --pod-cores 1 --core-threads 1 $(APP_PATH)/$(APP_NAME).so 7 1 1
