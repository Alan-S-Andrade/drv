DRV_DIR  ?= $(shell git rev-parse --show-toplevel)
SCRIPT   := $(DRV_DIR)/tests/riscvph.py
COMPILE_FLAGS += -nostartfiles
COMPILE_FLAGS += -I$(DRV_DIR)/riscv-examples/platform_ph
CFLAGS   += $(COMPILE_FLAGS)
CXXFLAGS += $(COMPILE_FLAGS)

LDFLAGS += -Wl,-T$(DRV_DIR)/riscv-examples/platform_ph/bsg_link.ld
LDFLAGS += -L$(DRV_DIR)/riscv-examples/platform_ph/pandohammer
#LIBS    += -lpandohammer

$(TARGET): crt.o

# $(TARGET): bsg_link.ld

# bsg_link.ld: $(DRV_DIR)/py/bsg_manycore_link_gen.py
# 	$(PYTHON) $< > $@


