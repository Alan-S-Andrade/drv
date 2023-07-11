# include the confing.mk
DRV_DIR ?= $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk

.PHONY: all install clean

LDFLAGS += -L$(DRV_LIB_DIR) -Wl,-rpath,$(DRV_LIB_DIR)
LDFLAGS += $(BOOST_LDFLAGS)

CXX := clang++

CXXFLAGS += $(BOOST_CXXFLAGS)
CXXFLAGS += -std=c++14 -fPIC -Wall -Wextra -Werror -Wno-unused-parameter
CXXFLAGS += -I$(DRV_INCLUDE_DIR)
CXXFLAGS += -I$(DRV_DIR)/api

libdrvapi-sources := $(wildcard $(DRV_DIR)/api/*.cpp)
libdrvapi-objects := $(patsubst %.cpp,%.o,$(libdrvapi-sources))
libdrvapi-headers := $(wildcard $(DRV_DIR)/api/*.hpp)

libdrvapi-install-headers := $(foreach header,$(libdrvapi-headers),$(DRV_INCLUDE_DIR)/$(notdir $(header)))

$(libdrvapi-install-headers): $(DRV_INCLUDE_DIR)
$(libdrvapi-install-headers): $(DRV_INCLUDE_DIR)/%: $(DRV_DIR)/api/%
	@cp $< $@

all: install
install: $(DRV_LIB_DIR)/libdrvapi.so $(libdrvapi-install-headers)

$(libdrvapi-objects): %.o: %.cpp $(libdrvapi-headers)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(DRV_LIB_DIR)/libdrvapi.so: $(DRV_INSTALL_DIR)
$(DRV_LIB_DIR)/libdrvapi.so: $(libdrvapi-objects)
	$(CXX) $(CXXFLAGS) -shared -fPIC -o $@ $(filter %.o,$^) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(libdrvapi-objects) $(DRV_LIB_DIR)/libdrvapi.so $(libdrvapi-install-headers)
