ifeq (, $(shell which sst-config))
 $(error "No sst-config in $(PATH), add `sst-config` to your PATH")
endif

CXX=$(shell sst-config --CXX)
CXXFLAGS=$(shell sst-config --ELEMENT_CXXFLAGS) -fno-stack-protector -O2
CXXFLAGS+=-DDEBUG -g
LDFLAGS=$(shell sst-config --ELEMENT_LDFLAGS) -fno-stack-protector
CPPFLAGS=-I./


drvsim-headers = $(wildcard *.hpp)
drvsim-sources = $(wildcard *.cpp)
drvsim-objects = $(patsubst %.cpp,%.o,$(drvsim-sources))

$(drvsim-objects): %.o: %.cpp $(drvsim-headers)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

libDrv.so: $(drvsim-objects) $(drvsim-headers)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS) -shared -o $@ $(drvsim-objects)

install: libDrv.so
	sst-register Drv Drv_LIBDIR=$(CURDIR)

clean:
	rm -Rf *.o *.so
