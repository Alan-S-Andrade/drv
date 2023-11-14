# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR := $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk
APP_NAME := to_native
SIM_OPTIONS += --pod-cores=1 --core-threads=1
SIM_THREADS = 2
#SIM_OPTIONS += --debug-requests
#SIM_OPTIONS += --verbose=10
include $(DRV_DIR)/mk/application_common.mk
