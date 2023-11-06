# SPDX-License-Identifier: MIT

DRV_DIR := $(shell git rev-parse --show-toplevel)
APP_NAME := app-main
include $(DRV_DIR)/mk/config.mk
include $(DRV_DIR)/mk/application_common.mk

run-%: $(APP_NAME).so
	@echo "see readme"
	sst $(DRV_DIR)/tests/drv-multicore-bus-test.py -- $(APP_PATH)/$(APP_NAME).so $(core) $(tthread) $(addr) $(n)
