ifeq (, $(shell which sst-config))
 $(error "No sst-config in $(PATH), add `sst-config` to your PATH")
endif

CXX=$(shell sst-config --CXX)
CXXFLAGS=$(shell sst-config --ELEMENT_CXXFLAGS) -fno-stack-protector -O2
CXXFLAGS+=-DDEBUG -g
LDFLAGS=$(shell sst-config --ELEMENT_LDFLAGS) -fno-stack-protector
CPPFLAGS=-I./


fwdsim-headers = $(wildcard *.hpp)
fwdsim-sources = $(wildcard *.cpp)
fwdsim-objects = $(patsubst %.cpp,%.o,$(fwdsim-sources))

$(fwdsim-objects): %.o: %.cpp $(fwdsim-headers)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

libFwd.so: $(fwdsim-objects) $(fwdsim-headers)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS) -shared -o $@ $(fwdsim-objects)

install: libFwd.so
	sst-register Fwd Fwd_LIBDIR=$(CURDIR)

clean:
	rm -Rf *.o *.so
