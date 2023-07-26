DRV_DIR ?= $(shell git rev-parse --show-toplevel)
ifndef _DRV_CONFIG_MK
_DRV_CONFIG_MK := 1
include $(DRV_DIR)/mk/sst_config.mk
include $(DRV_DIR)/mk/boost_config.mk
include $(DRV_DIR)/mk/install_config.mk
include $(DRV_DIR)/mk/application_config.mk
endif
