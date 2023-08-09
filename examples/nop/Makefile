APP_NAME := nop
DRV_DIR := $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk
include $(DRV_DIR)/mk/application_common.mk

run-%: $(APP_NAME).so
	$(eval cycles=$(word 1,$(subst -, ,$*)))
	@echo Cycles=$(cycles)
	sst $(DRV_DIR)/tests/drv-multicore-bus-test.py -- $(APP_PATH)/$(APP_NAME).so $(cycles)
