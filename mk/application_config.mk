DRV_DIR ?= $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/boost_config.mk

APP_CXX := clang++
APP_CC  := clang

APP_CCXXFLAGS += -Wall -Wextra -Werror -Wno-vla-extension -Wno-unused-parameter -O2
APP_CCXXFLAGS += -Wno-unused-variable -Wno-unused-but-set-variable -Wno-gnu-zero-variadic-macro-arguments
APP_CCXXFLAGS += -fPIC
APP_CCXXFLAGS += -I$(DRV_DIR)/install/include
APP_CCXXFLAGS += $(BOOST_CXXFLAGS)

APP_CXXFLAGS += $(APP_CCXXFLAGS)
APP_CXXFLAGS += -std=c++11

APP_CFLAGS += $(APP_CCXXFLAGS)
APP_CFLAGS += -std=c11

APP_LDFLAGS += -L$(DRV_DIR)/install/lib -Wl,-rpath,$(DRV_DIR)/install/lib
APP_LDFLAGS += -shared
APP_LDFLAGS += $(BOOST_LDFLAGS)

APP_LIBS += -lboost_coroutine -lboost_context
APP_LIBS += -ldrvapi #-ldrvapiapp
