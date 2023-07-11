DRV_DIR = $(shell git rev-parse --show-toplevel)

all: install examples

EXAMPLES = $(wildcard $(DRV_DIR)/examples/*/)

.PHONY: all install clean debug examples $(EXAMPLES)

install: 
	$(MAKE) -C $(DRV_DIR)/api/ install
	$(MAKE) -C $(DRV_DIR)/element/ install

clean:
	$(MAKE) -C $(DRV_DIR)/element/ clean
	$(MAKE) -C $(DRV_DIR)/api/ clean

debug:
	@echo $(EXAMPLES)

examples: $(EXAMPLES)

$(EXAMPLES):
	$(MAKE) -C $@ all
