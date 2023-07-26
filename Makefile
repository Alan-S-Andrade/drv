DRV_DIR = $(shell git rev-parse --show-toplevel)

all: install examples

EXAMPLES = $(wildcard $(DRV_DIR)/examples/*/)

.PHONY: all install install-element install-api
.PHONY: clean  examples $(EXAMPLES)

install: install-api install-element

install-api:
	$(MAKE) -C $(DRV_DIR)/api/ install

install-element: install-api
	$(MAKE) -C $(DRV_DIR)/element/ install

$(foreach example, $(EXAMPLES), $(example)-clean): %-clean:
	$(MAKE) -C $* clean

clean: $(foreach example, $(EXAMPLES), $(example)-clean)
	$(MAKE) -C $(DRV_DIR)/element/ clean
	$(MAKE) -C $(DRV_DIR)/api/ clean
	rm -rf install/

$(EXAMPLES): install-api install-element

examples: $(EXAMPLES)

$(EXAMPLES):
	$(MAKE) -C $@ all
