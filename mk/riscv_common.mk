ifndef _RISCV_COMMON_MK_
_RISCV_COMMON_MK_ := 1

DRV_DIR := $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk

CXX := $(RISCV_INSTALL_DIR)/bin/riscv64-$(RISCV_ARCH)-g++
CC  := $(RISCV_INSTALL_DIR)/bin/riscv64-$(RISCV_ARCH)-gcc

PLATFORM ?= default

vpath %.S $(DRV_DIR)/riscv-examples/platform_$(PLATFORM)
vpath %.c $(DRV_DIR)/riscv-examples/platform_$(PLATFORM)
vpath %.cpp $(DRV_DIR)/riscv-examples/platform_$(PLATFORM)

ARCH:=rv64imafd
ABI:=lp64d

COMPILE_FLAGS += -O2 -march=$(ARCH) -mabi=$(ABI)
CXXFLAGS += $(COMPILE_FLAGS)
CFLAGS   += $(COMPILE_FLAGS)
LDFLAGS  +=
LIBS     +=

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
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(filter %.o,$^) $(LIBS)

.PHONY: clean
clean:
	rm -f *.o *.riscv

SIM_OPTIONS ?=

run: $(TARGET)
	sst $(SCRIPT) -- $(TARGET) $(SIM_OPTIONS)
endif
