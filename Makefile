all: element

.PHONY: element

element:
	$(MAKE) -C $@/ install
