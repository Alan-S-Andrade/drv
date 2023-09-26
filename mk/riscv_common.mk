#SST_RISCV_DIR := $(shell git rev-parse --show-toplevel)
#include $(SST_RISCV_DIR)/mk/config.mk
DRV_DIR := $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk

CXX := $(RISCV_INSTALL_DIR)/bin/riscv64-unknown-elf-g++
CC  := $(RISCV_INSTALL_DIR)/bin/riscv64-unknown-elf-gcc

PLATFORM ?= default

vpath %.S $(DRV_DIR)/riscv-examples/platform_$(PLATFORM)
vpath %.c $(DRV_DIR)/riscv-examples/platform_$(PLATFORM)
vpath %.cpp $(DRV_DIR)/riscv-examples/platform_$(PLATFORM)

ARCH:=rv64imafd
ABI:=lp64d

CXXFLAGS := -O2  -march=$(ARCH) -mabi=$(ABI)
CFLAGS   := -O2  -march=$(ARCH) -mabi=$(ABI)
LDFLAGS  :=

-include $(DRV_DIR)/riscv-examples/platform_$(PLATFORM)/common.mk

CSOURCE   += $(wildcard *.c)
COBJECT   += $(patsubst %.c,%.o,$(CSOURCE))
CXXSOURCE += $(wildcard *.cpp)

CXXOBJECT := $(patsubst %.cpp,%.o,$(CXXSOURCE))
ASMSOURCE := $(wildcard *.S)
ASMOBJECT := $(patsubst %.S,%.o,$(ASMSOURCE))

$(ASMOBJECT): %.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

$(COBJECT): %.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(CXXOBJECT): %.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(TARGET): %.riscv: $(COBJECT) $(CXXOBJECT) $(ASMOBJECT)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(filter %.o,$^)

.PHONY: clean
clean:
	rm -f *.o *.riscv

SIM_OPTIONS ?=

run: $(TARGET)
	sst $(SCRIPT) -- $(TARGET) $(SIM_OPTIONS)
