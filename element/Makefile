ifeq (, $(shell which sst-config))
 $(error "No sst-config in $(PATH), add `sst-config` to your PATH")
endif

# include the config.mk
DRV_DIR=$(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk

CXX=$(shell sst-config --CXX)
CXXFLAGS=$(shell sst-config --ELEMENT_CXXFLAGS) -fno-stack-protector -O2
CXXFLAGS+=$(BOOST_CXXFLAGS)
CXXFLAGS+=-DDEBUG -g

LDFLAGS =  $(shell sst-config --ELEMENT_LDFLAGS) -fno-stack-protector
LDFLAGS += -L$(DRV_LIB_DIR) -Wl,-rpath,$(DRV_LIB_DIR)
LDFLAGS += $(BOOST_LDFLAGS)


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
	sst-register Drv Drv_LIBDIR=$(CURDIR)

clean:
	rm -Rf *.o *.so
