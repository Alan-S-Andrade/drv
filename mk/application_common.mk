ifndef APP_NAME
$(error APP_NAME is not set)
endif

DRV_DIR ?= $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk

APP_PATH ?= $(DRV_DIR)/examples/$(APP_NAME)

# Build options
CXX ?= clang++
CC  ?= clang

CXXFLAGS := $(APP_CXXFLAGS)
CFLAGS   := $(APP_CFLAGS)

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
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# build the app as a shared object
$(APP_NAME).so: $(APP_NAME).o
$(APP_NAME).so:
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.o,$^) $(LDFLAGS) $(LIBS)


SCRIPT ?= drv-multicore-bus-test.py
.PHONY: run
run: $(APP_NAME).so
	sst $(DRV_DIR)/tests/$(SCRIPT) -- $(APP_PATH)/$(APP_NAME).so
