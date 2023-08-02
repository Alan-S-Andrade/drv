DRV_DIR := $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/boost_config.mk

APP_CXX := clang++

APP_CXXFLAGS := -std=c++11 -Wall -Wextra -Werror -Wno-vla-extension -pedantic -Wno-unused-parameter -O2
APP_CXXFLAGS += -Wno-unused-variable
APP_CXXFLAGS += -fPIC
APP_CXXFLAGS += -I$(DRV_DIR)/install/include
APP_CXXFLAGS += $(BOOST_CXXFLAGS)

APP_LDFLAGS := -L$(DRV_DIR)/install/lib -Wl,-rpath,$(DRV_DIR)/install/lib
APP_LDFLAGS += -shared
APP_LDFLAGS += $(BOOST_LDFLAGS)

APP_LIBS := -lboost_coroutine -lboost_context
APP_LIBS += -ldrvapi #-ldrvapiapp
