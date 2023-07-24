DRV_DIR ?= $(shell git rev-parse --show-toplevel)
SST_ELEMENTS_INSTALL_DIR=$(HOME)/work/AGILE/sst-elements/install
SST_ELEMENTS_CXXFLAGS += -I$(SST_ELEMENTS_INSTALL_DIR)/include
SST_ELEMENTS_LDFLAGS += -L$(SST_ELEMENTS_INSTALL_DIR)/lib -Wl,-rpath,$(SST_ELEMENTS_INSTALL_DIR)/lib
