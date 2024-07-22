# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR := $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk
APP_NAME := new_addrmap
SIM_OPTIONS := --num-pxn=2 --pxn-pods=2 --pod-cores=2 --core-threads=1
include $(DRV_DIR)/mk/application_common.mk

