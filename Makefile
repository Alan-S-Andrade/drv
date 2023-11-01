# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR = $(shell git rev-parse --show-toplevel)

all: install examples

EXAMPLES = $(wildcard $(DRV_DIR)/examples/*/)

.PHONY: all install install-element install-api install-interpreter
.PHONY: clean  examples $(EXAMPLES)

install: install-api install-element install-interpreter

install-api:
	$(MAKE) -C $(DRV_DIR)/api/ install

install-interpreter:
	$(MAKE) -C $(DRV_DIR)/interpreter/ install

install-element: install-api install-interpreter
	$(MAKE) -C $(DRV_DIR)/element/ install

$(foreach example, $(EXAMPLES), $(example)-clean): %-clean:
	$(MAKE) -C $* clean

clean: $(foreach example, $(EXAMPLES), $(example)-clean)
	$(MAKE) -C $(DRV_DIR)/element/ clean
	$(MAKE) -C $(DRV_DIR)/api/ clean
	$(MAKE) -C $(DRV_DIR)/interpreter/ clean
	rm -rf install/

$(EXAMPLES): install-api install-element install-interpreter

examples: $(EXAMPLES)

$(EXAMPLES):
	$(MAKE) -C $@ all
