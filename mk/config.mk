DRV_DIR ?= $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/boost_config.mk
include $(DRV_DIR)/mk/install_config.mk
include $(DRV_DIR)/mk/application_config.mk
