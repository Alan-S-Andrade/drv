# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

BFS_DIR := $(shell git rev-parse --show-toplevel)
DRV_DIR := $(BFS_DIR)/../..
include $(DRV_DIR)/mk/config.mk

APP_NAME := bfs
APP_PATH := $(BFS_DIR)/push-pull

APP_CCXXFLAGS += -I$(BFS_DIR)/sparse_matrix_helpers

include $(DRV_DIR)/mk/application_common.mk

vpath %.cpp $(BFS_DIR)/sparse_matrix_helpers
vpath %.c   $(BFS_DIR)/sparse_matrix_helpers

$(APP_NAME).so: mmio.o
$(APP_NAME).so: breadth_first_search_graph.o
$(APP_NAME).so: read_graph.o
$(APP_NAME).so: transpose_graph.o

run-%: $(APP_NAME).so
	$(eval graph=$(word 1, $(subst -, ,$*)))
	$(eval root=$(word 2, $(subst -, ,$*)))
	$(MAKE) -C $(BFS_DIR)/sparse-inputs $(graph).mtx
	sst $(DRV_DIR)/tests/$(SCRIPT) -- \
	$(SIM_OPTIONS) $(APP_PATH)/$(APP_NAME).so \
	$(BFS_DIR)/sparse-inputs/$(graph).mtx \
	$(root)

clean: clean-objects
clean-objects:
	rm -f *.o *.so
