DRV_DIR := $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk

# change me: the app name
APP_NAME := comm
APP_PATH := $(DRV_DIR)/examples/$(APP_NAME)

# Build options
CXX := clang++

CXXFLAGS := $(APP_CXXFLAGS)

LDFLAGS := $(APP_LDFLAGS)
LDFLAGS += -Wl,-rpath=$(APP_PATH)

LIBS := $(APP_LIBS)

.PHONY: all clean

all: $(APP_NAME).so

# clean rule
clean:
	rm -f $(APP_NAME).so $(APP_NAME).o

# generic object file build rule
%.o: %.cpp
	@echo CXXFLAGS=$(CXXFLAGS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# build the app as a shared object
$(APP_NAME).so: $(APP_NAME).o
$(APP_NAME).so:
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.o,$^) $(LDFLAGS) $(LIBS)

