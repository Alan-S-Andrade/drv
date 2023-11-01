# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR := $(shell git rev-parse --show-toplevel)
APP_NAME := latency
include $(DRV_DIR)/mk/config.mk
include $(DRV_DIR)/mk/application_common.mk

run-%: $(APP_NAME).so
	$(eval core=0)
	$(eval tthread=0)
	$(eval addr=$(word 1,$(subst -, ,$*)))
	$(eval n=$(word 2,$(subst -, ,$*)))
	sst $(DRV_DIR)/tests/$(SCRIPT) -- $(APP_PATH)/$(APP_NAME).so $(core) $(tthread) $(addr) $(n)
