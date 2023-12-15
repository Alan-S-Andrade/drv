# SPDX-License-Identifier: MIT

DRV_DIR := $(shell git rev-parse --show-toplevel)
APP_NAME := IDM 
include $(DRV_DIR)/mk/config.mk
include $(DRV_DIR)/mk/application_common.mk

run-%: $(APP_NAME).so
	@echo "see readme"
