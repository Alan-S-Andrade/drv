# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

ifeq (, $(shell which sst-config))
 $(error "No sst-config in $(PATH), add `sst-config` to your PATH")
endif
SST_CONFIG   := $(shell which sst-config)
SST_REGISTER := $(shell which sst-register)

# include the config.mk
DRV_DIR=$(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk

CXX=$(shell $(SST_CONFIG) --CXX)
CXXFLAGS=$(shell $(SST_CONFIG) --ELEMENT_CXXFLAGS) -fno-stack-protector -O2
CXXFLAGS+=$(BOOST_CXXFLAGS)
CXXFLAGS+=$(SST_ELEMENTS_CXXFLAGS)
CXXFLAGS+=-I$(RISCV_INSTALL_DIR)
CXXFLAGS+=-Wall -Wextra -Werror
CXXFLAGS+=-Wno-reorder -Wno-ignored-qualifiers -Wno-unused-parameter
CXXFLAGS+=-DDEBUG -g

LDFLAGS =  $(shell sst-config --ELEMENT_LDFLAGS) -fno-stack-protector
LDFLAGS += -L$(DRV_LIB_DIR) -Wl,-rpath,$(DRV_LIB_DIR)
LDFLAGS += $(BOOST_LDFLAGS)
LDFLAGS += $(SST_ELEMENTS_LDFLAGS)


CXXFLAGS += -I$(DRV_DIR)/element/
CXXFLAGS += -I$(DRV_INCLUDE_DIR)

LIBS += -lboost_coroutine -lboost_context -ldrvapi

drvsim-headers  = $(wildcard *.hpp)
drvsim-sources = $(wildcard *.cpp)
drvsim-objects = $(patsubst %.cpp,%.o,$(drvsim-sources))

$(drvsim-objects): %.o: %.cpp $(drvsim-headers)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@


libDrv.so: $(drvsim-objects) $(drvsim-headers)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS) -shared -o $@ $(drvsim-objects) $(LIBS)

install: libDrv.so
	$(SST_REGISTER) Drv Drv_LIBDIR=$(CURDIR)

clean:
	rm -Rf *.o *.so
