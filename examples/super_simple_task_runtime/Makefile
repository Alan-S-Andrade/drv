# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

APP_NAME := runtime
DRV_DIR := $(shell git rev-parse --show-toplevel)
APP_PATH := $(DRV_DIR)/examples/super_simple_task_runtime

include $(DRV_DIR)/mk/config.mk
include $(DRV_DIR)/mk/application_common.mk

$(APP_NAME).so: main.o
