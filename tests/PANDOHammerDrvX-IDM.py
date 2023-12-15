# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

########################################################################################################
# Diagram of this model:                                                                               #
# https://docs.google.com/presentation/d/1ekm0MbExI1PKca5tDkSGEyBi-_0000Ro9OEaBLF-rUQ/edit?usp=sharing #
########################################################################################################
from drv import *
from drv_memory import *
import sys

# for drvx we set the core clock to 125MHz (assumption is 1/8 ops are memory)
SYSCONFIG["sys_core_clock"] = "125MHz"

# SYSCONFIG["sys_pod_cores"] = 2
# SYSCONFIG["sys_core_threads"] = 1 

image = "./../examples/IDM/inputs/Data01CSR.bin"

print("""
PANDOHammerDrvX:
  program = {}
""".format(
    arguments.program
))

class Tile(object):
    def l1sp_start(self):
        return self.l1sprange.start

    def l1sp_end(self):
        return self.l1sprange.end

    def markAsLoader(self):
        """
        Set this tile as responsible for loading the executable.
        """
        self.core.addParams({"load" : 1})

    def initMem(self):
        """
        Initialize the tile's scratchpad memory
        """
        # create the scratchpad
        self.scratchpad_mectrl = sst.Component("scratchpad_mectrl_%d" % self.id, "memHierarchy.MemController")
        self.scratchpad_mectrl.addParams({
            "debug" : 0,
            "debug_level" : 0,
            "verbose" : 0,
            "clock" : "1GHz",
            "addr_range_start" : self.l1sp_start(),
            "addr_range_end" :   self.l1sp_end(),
        })
        # set the backend memory system to Drv special memory
        # (needed for AMOs)
        self.scratchpad = self.scratchpad_mectrl.setSubComponent("backend", "Drv.DrvSimpleMemBackend")
        self.scratchpad.addParams({
            "verbose_level" : arguments.verbose_memory,
            "access_time" : "1ns",
            "mem_size" : "4KiB",
        })

        # set the custom command handler
        # we need to use the Drv custom command handler
        # to handle our custom commands (AMOs)
        self.scratchpad_customcmdhandler = self.scratchpad_mectrl.setSubComponent("customCmdHandler", "Drv.DrvCmdMemHandler")
        self.scratchpad_customcmdhandler.addParams({
            "verbose_level" : arguments.verbose_memory,
        })
        self.scratchpad_nic = self.scratchpad_mectrl.setSubComponent("cpulink", "memHierarchy.MemNIC")
        self.scratchpad_nic.addParams({
            "group" : 1,
            "network_bw" : "1024GB/s",
        })

    def initRtr(self):
        """
        Initialize the tile's router
        """
        # create the tile network
        self.tile_rtr = sst.Component("tile_rtr_%d" % self.id, "merlin.hr_router")
        self.tile_rtr.addParams({
            # semantics parameters
            "id" : COMPUTETILE_RTR_ID + self.id,
            "num_ports" : 3,
            "topology" : "merlin.singlerouter",
            # performance models
            "xbar_bw" : "1024GB/s",
            "link_bw" : "1024GB/s",
            "flit_size" : "8B",
            "input_buf_size" : "1KB",
            "output_buf_size" : "1KB",
            "debug" : 1,
        })

        # setup connection rtr <-> core
        self.tile_rtr.setSubComponent("topology","merlin.singlerouter")
        self.core_nic_link = sst.Link("core_nic_link_%d" % self.id)
        self.core_nic_link.connect(
            (self.core_nic, "port", "1ns"),
            (self.tile_rtr, "port0", "1ns")
        )

        # setup connection rtr <-> scratchpad
        self.scratchpad_nic_link = sst.Link("scratchpad_nic_link_%d" % self.id)
        self.scratchpad_nic_link.connect(
            (self.scratchpad_nic, "port", "1ns"),
            (self.tile_rtr, "port1", "1ns")
        )

    def initCore(self):
        """
        Initialize the tile's core
        """
        # create the core
        self.core = sst.Component("core_%d" % self.id, "Drv.DrvCore")
        self.core.addParams({
            "verbose"   : arguments.verbose,
            "threads"   : SYSCONFIG["sys_core_threads"],
            "clock"     : SYSCONFIG["sys_core_clock"],
            "executable": arguments.program,
            "argv" : ' '.join(arguments.argv),
            "max_idle" : 100//8, # turn clock offf after idle for 1 us
            "id"  : self.id,
            "pod" : self.pod,
            "pxn" : self.pxn,
        })
        self.core.addParams(SYSCONFIG)
        self.core.addParams(CORE_DEBUG)

        self.core_mem = self.core.setSubComponent("memory", "Drv.DrvStdMemory")
        self.core_mem.addParams({
            "verbose" : arguments.verbose,
        })

        self.core_iface = self.core_mem.setSubComponent("memory", "memHierarchy.standardInterface")
        self.core_iface.addParams({
            "verbose" : arguments.verbose,
        })
        self.core_nic = self.core_iface.setSubComponent("memlink", "memHierarchy.MemNIC")
        self.core_nic.addParams({
            "group" : 0,
            "network_bw" : "1024GB/s",
            "destinations" : "1,2",
            "verbose_level" : arguments.verbose_memory,
        })

    def __init__(self, id, pod=0, pxn=0):
        self.id = id
        self.pod = pod
        self.pxn = pxn
        self.l1sprange = L1SPRange(self.pxn, self.pod, self.id >> 3, self.id & 0x7)
        """
        Create a tile with the given ID, pod, and PXN.
        """
        self.initCore()
        self.initMem()
        self.initRtr()


# build the tiles
tiles = []
CORES = SYSCONFIG["sys_pod_cores"]
for i in range(CORES):
    tiles.append(Tile(i))

tiles[0].markAsLoader()

# build the shared memory
POD_L2_BANKS = SYSCONFIG["sys_pod_l2_banks"]
l2_banks = []
for i in range(POD_L2_BANKS):
    l2_banks.append(L2MemoryBank(i))

# build the main memory banks
POD_MAINMEM_BANKS = SYSCONFIG["sys_pod_dram_ports"]
mainmem_banks = []
# mainmem_banks.append(MainMemoryBank(0, preload=image))

for i in range(POD_MAINMEM_BANKS - 2):
    mainmem_banks.append(MainMemoryBank(i))
    
mainmem_banks.append(MainMemoryBank(POD_MAINMEM_BANKS - 2, preload=image))
mainmem_banks.append(MainMemoryBank(POD_MAINMEM_BANKS - 1, preload=image))


# for m in mainmem_banks:
#     print("0x{:16x}".format(m.address_range.start))
  


# build the network crossbar
chiprtr = sst.Component("chiprtr", "merlin.hr_router")
chiprtr.addParams({
    # semantics parameters
    "id" : CHIPRTR_ID,
    "num_ports" : CORES+POD_L2_BANKS+POD_MAINMEM_BANKS,
    "topology" : "merlin.singlerouter",
    # performance models
    "xbar_bw" : "256GB/s",
    "link_bw" : "256GB/s",
    "flit_size" : "8B",
    "input_buf_size" : "1KB",
    "output_buf_size" : "1KB",
})
chiprtr.setSubComponent("topology","merlin.singlerouter")

# wire up the tiles network
for (i, tile) in enumerate(tiles):
    bridge = sst.Component("bridge_%d" % i, "merlin.Bridge")
    bridge.addParams({
        "translator" : "memHierarchy.MemNetBridge",
        "debug" : 1,
        "debug_level" : 10,
        "network_bw" : "256GB/s",
    })
    tile_bridge_link = sst.Link("tile_bridge_link_%d" % i)
    tile_bridge_link.connect(
        (bridge, "network0", "1ns"),
        (tile.tile_rtr, "port2", "1ns")
    )
    bridge_chiprtr_link = sst.Link("bridge_chiprtr_link_%d" % i)
    bridge_chiprtr_link.connect(
        (bridge, "network1", "1ns"),
        (chiprtr, "port%d" % i, "1ns")
    )

# wire up the shared memory
base_l2_bankno = len(tiles)
for (i, l2_bank) in enumerate(l2_banks):
    bridge = sst.Component("bridge_%d" % (base_l2_bankno+i), "merlin.Bridge")
    bridge.addParams({
        "translator" : "memHierarchy.MemNetBridge",
        "debug" : 1,
        "debug_level" : 10,
        "network_bw" : "256GB/s",
        })
    l2_bank_bridge_link = sst.Link("l2bank_bridge_link_%d" % i)
    l2_bank_bridge_link.connect(
        (bridge, "network0", "1ns"),
        (l2_bank.mem_rtr, "port1", "1ns")
    )
    bridge_chiprtr_link = sst.Link("bridge_chip_memrtr_link_%d" %i)
    bridge_chiprtr_link.connect(
        (bridge, "network1", "1ns"),
        (chiprtr, "port%d" % (base_l2_bankno+i), "1ns")
    )

# wire up the main memory
base_mainmem_bankno = base_l2_bankno + len(l2_banks)
for (i, mainmem_bank) in enumerate(mainmem_banks):
    bridge = sst.Component("mainmem_bridge_%d" % i, "merlin.Bridge")
    bridge.addParams({
        "translator" : "memHierarchy.MemNetBridge",
        "debug" : 1,
        "debug_level" : 10,
        "network_bw" : "256GB/s",
    })
    mainmem_bank_bridge_link = sst.Link("mainmem_bank_bridge_link_%d" % i)
    lat = '1ns'
    if i == len(mainmem_banks) - 1:
      lat = '1us'
    mainmem_bank_bridge_link.connect(
        (bridge, "network0", lat),
        (mainmem_bank.mem_rtr, "port1", "1ns")
    )
    bridge_chiprtr_link = sst.Link("bridge_chip_mainmem_memrtr_link_%d" %i)
    bridge_chiprtr_link.connect(
        (bridge, "network1", "1ns"),
        (chiprtr, "port%d" % (base_mainmem_bankno+i), "1ns")
    )



