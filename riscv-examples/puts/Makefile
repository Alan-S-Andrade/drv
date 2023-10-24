DRV_DIR := $(shell git rev-parse --show-toplevel)

TARGET := exe.riscv
PLATFORM ?= ph

SIM_OPTIONS += --verbose 0
# SIM_OPTIONS += --debug-memory
# SIM_OPTIONS += --debug-responses
# SIM_OPTIONS += --debug-requests
SIM_OPTIONS += --verbose-memory 0

include $(DRV_DIR)/mk/riscv_common.mk
