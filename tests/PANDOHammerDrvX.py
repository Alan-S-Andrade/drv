# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

# Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
# Included multi node simulation

########################################################################################################
# Diagram of this model:                                                                               #
# https://docs.google.com/presentation/d/1ekm0MbExI1PKca5tDkSGEyBi-_0000Ro9OEaBLF-rUQ/edit?usp=sharing #
########################################################################################################
from drv import *
from drv_memory import *

# for drvx we set the core clock to 125MHz (assumption is 1/8 ops are memory)
SYSCONFIG["sys_core_clock"] = "125MHz"

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
        self.scratchpad_mectrl = sst.Component("scratchpad_mectrl_%d_pod%d_pxn%d" % (self.id, self.pod, self.pxn), "memHierarchy.MemController")
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
            "mem_size" : L1SPRange.L1SP_SIZE_STR,
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
        self.tile_rtr = sst.Component("tile_rtr_%d_pod%d_pxn%d" % (self.id, self.pod, self.pxn), "merlin.hr_router")
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
        self.core_nic_link = sst.Link("core_nic_link_%d_pod%d_pxn%d" % (self.id, self.pod, self.pxn))
        self.core_nic_link.connect(
            (self.core_nic, "port", "1ns"),
            (self.tile_rtr, "port0", "1ns")
        )

        # setup connection rtr <-> scratchpad
        self.scratchpad_nic_link = sst.Link("scratchpad_nic_link_%d_pod%d_pxn%d" % (self.id, self.pod, self.pxn))
        self.scratchpad_nic_link.connect(
            (self.scratchpad_nic, "port", "1ns"),
            (self.tile_rtr, "port1", "1ns")
        )

    def initCore(self):
        """
        Initialize the tile's core
        """
        # create the core
        self.core = sst.Component("core_%d_pod%d_pxn%d" % (self.id, self.pod, self.pxn), "Drv.DrvCore")
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
            "verbose"           : arguments.verbose,
            "verbose_init"      : arguments.debug_init,
            "verbose_requests"  : arguments.debug_requests,
            "verbose_responses" : arguments.debug_responses,
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


PXNS = SYSCONFIG["sys_num_pxn"]
PODS = SYSCONFIG["sys_pxn_pods"]
CORES = SYSCONFIG["sys_pod_cores"]
POD_L2_BANKS = SYSCONFIG["sys_pod_l2_banks"]
PXN_MAINMEM_BANKS = SYSCONFIG["sys_pod_dram_ports"]

onchiprtr = []
# build pxns
for pxn in range(PXNS):
    # build the main memory banks
    mainmem_banks = []
    for i in range(PXN_MAINMEM_BANKS):
        mainmem_banks.append(MainMemoryBank(i,pxn))

    # build the network crossbar
    chiprtr = sst.Component("chiprtr%d" % pxn, "merlin.hr_router")
    chiprtr.addParams({
        # semantics parameters
        "id" : CHIPRTR_ID + pxn,
        "num_ports" : PODS * (CORES + POD_L2_BANKS) + (1 if arguments.with_command_processor else 0) + PXN_MAINMEM_BANKS + PXNS - 1, # If number of PXNS is equal to 1 we do not need additional port. Hence -1 
        "topology" : "merlin.singlerouter",
        # performance models
        "xbar_bw" : "256GB/s",
        "link_bw" : "256GB/s",
        "flit_size" : "8B",
        "input_buf_size" : "1KB",
        "output_buf_size" : "1KB",
    })
    chiprtr.setSubComponent("topology","merlin.singlerouter")
    onchiprtr.append(chiprtr)

    # build pods
    for pod in range(PODS):
        # build the tiles
        tiles = []
        for i in range(CORES):
            tiles.append(Tile(i,pod,pxn))

        tiles[0].markAsLoader()

        # build the shared memory
        l2_banks = []
        for i in range(POD_L2_BANKS):
            l2_banks.append(L2MemoryBank(i,pod,pxn))

        # wire up the tiles network
        base_core_no = pod*(CORES+POD_L2_BANKS)
        for (i, tile) in enumerate(tiles):
            bridge = sst.Component("bridge_%d_pod%d_pxn%d" % (i, pod, pxn), "merlin.Bridge")
            bridge.addParams({
                "translator" : "memHierarchy.MemNetBridge",
                "debug" : 1,
                "debug_level" : 10,
                "network_bw" : "256GB/s",
            })
            tile_bridge_link = sst.Link("tile_bridge_link_%d_pod%d_pxn%d" % (i, pod, pxn))
            tile_bridge_link.connect(
                (bridge, "network0", "1ns"),
                (tile.tile_rtr, "port2", "1ns")
            )
            bridge_chiprtr_link = sst.Link("bridge_chiprtr_link_%d_pod%d_pxn%d" % (i, pod, pxn))
            bridge_chiprtr_link.connect(
                (bridge, "network1", "1ns"),
                (chiprtr, "port%d" % (base_core_no+i), "1ns")
            )

        # wire up the shared memory
        base_l2_bankno = pod*(CORES+POD_L2_BANKS)+len(tiles)
        for (i, l2_bank) in enumerate(l2_banks):
            bridge = sst.Component("bridge_%d_pod%d_pxn%d" % (base_l2_bankno+i, pod, pxn), "merlin.Bridge")
            bridge.addParams({
                "translator" : "memHierarchy.MemNetBridge",
                "debug" : 1,
                "debug_level" : 10,
                "network_bw" : "256GB/s",
            })
            l2_bank_bridge_link = sst.Link("l2bank_bridge_link_%d_pod%d_pxn%d" % (i, pod, pxn))
            l2_bank_bridge_link.connect(
                (bridge, "network0", "1ns"),
                (l2_bank.mem_rtr, "port1", "1ns")
            )
            bridge_chiprtr_link = sst.Link("bridge_chip_memrtr_link_%d_pod%d_pxn%d" % (i, pod, pxn))
            bridge_chiprtr_link.connect(
                (bridge, "network1", "1ns"),
                (chiprtr, "port%d" % (base_l2_bankno+i), "1ns")
            )

    # wire up the main memory
    base_mainmem_bankno = PODS*(CORES+POD_L2_BANKS)
    for (i, mainmem_bank) in enumerate(mainmem_banks):
        bridge = sst.Component("mainmem_bridge_%d_pxn%d" % (i, pxn), "merlin.Bridge")
        bridge.addParams({
            "translator" : "memHierarchy.MemNetBridge",
            "debug" : 1,
            "debug_level" : 10,
            "network_bw" : "256GB/s",
        })
        mainmem_bank_bridge_link = sst.Link("mainmem_bank_bridge_link_%d_pxn%d" % (i, pxn))
        mainmem_bank_bridge_link.connect(
            (bridge, "network0", "1ns"),
            (mainmem_bank.mem_rtr, "port1", "1ns")
        )
        bridge_chiprtr_link = sst.Link("bridge_chip_mainmem_memrtr_link_%d_pxn%d" % (i, pxn))
        bridge_chiprtr_link.connect(
            (bridge, "network1", "1ns"),
            (chiprtr, "port%d" % (base_mainmem_bankno+i), "1ns")
        )

    # wire up the command processor
    if arguments.with_command_processor:
        command_processor = CommandProcessor(pod,pxn)
        chiprtr_command_processor_link = sst.Link("chiprtr_command_processor_link_pod%d_pxn%d" % (pod, pxn))
        chiprtr_command_processor_link.connect(
            (chiprtr, "port%d" % (PODS*(CORES+POD_L2_BANKS) + PXN_MAINMEM_BANKS), "1ns"),
            (command_processor.core_nic, "port", "1ns")
        )

# build off-chip network crossbar
if (PXNS > 1):
    offchiprtr = sst.Component("offchiprtr", "merlin.hr_router")
    offchiprtr.addParams({
        # semantics parameters
        "id" : PXNS,
        "num_ports" : PXNS,
        "topology" : "merlin.singlerouter",
        # performance models
        "xbar_bw" : "256GB/s",
        "link_bw" : "256GB/s",
        "flit_size" : "8B",
        "input_buf_size" : "1KB",
        "output_buf_size" : "1KB",
    })
    offchiprtr.setSubComponent("topology","merlin.singlerouter")

    # wire up off chip and on chip rtrs
    for pxn in range(PXNS):
        shared_mem_port = PODS*(CORES+POD_L2_BANKS) + PXN_MAINMEM_BANKS + (1 if arguments.with_command_processor else 0)
        bridge = sst.Component("offchiprtr_bridge_pxn_%d" % pxn, "merlin.Bridge")
        bridge.addParams({
            "translator" : "memHierarchy.MemNetBridge",
            "debug" : 1,
            "debug_level" : 10,
            "network_bw" : "256GB/s",
        })
        onchiprtr_bridge_link = sst.Link("onchiprtr_bridge_link_pxn_%d" % pxn)
        onchiprtr_bridge_link.connect(
            (bridge, "network0", "1ns"),
            (onchiprtr[pxn], "port%d" % shared_mem_port, "1ns")
        )
        offchiprtr_bridge_chiprtr_link = sst.Link("bridge_offchiprtr_link_pxn_%d" % pxn)
        offchiprtr_bridge_chiprtr_link.connect(
            (bridge, "network1", "1ns"),
            (offchiprtr, "port%d" % (pxn), "1ns")
        )

