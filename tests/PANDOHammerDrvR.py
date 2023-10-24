########################################################################################################
# Diagram of this model:                                                                               #
# https://docs.google.com/presentation/d/1FnrAjOXJKo5vKgo7IkuSD7QT15aDAmJi5Pts6IQhkX8/edit?usp=sharing #
########################################################################################################
import sst
import argparse

COMPUTETILE_RTR_ID = 0
SHAREDMEM_RTR_ID = 1024
CHIPRTR_ID = 1024*1024

SYSCONFIG = {
    "sys_num_pxn" : 1,
    "sys_pxn_pods" : 1,
    "sys_pod_cores" : 8,
    "sys_core_threads" : 16,
    "sys_core_clock" : "1GHz",
    "sys_pod_dram_ports" : 2,
    "sys_nw_flit_dwords" : 1,
    "sys_nw_obuf_dwords" : 8,
}

CORE_DEBUG = {
    "debug_init" : False,
    "debug_clock" : False,
    "debug_requests" : False,
    "debug_responses" : False,
    "debug_loopback" : False,
    "debug_memory": False,
    "debug_syscalls" : False,
}

# parse command line arguments
parser = argparse.ArgumentParser()
parser.add_argument("program", help="program to run")
parser.add_argument("--verbose", type=int, default=0, help="verbosity of core")
parser.add_argument("--dram-backend", type=str, default="simple", choices=['simple', 'ramulator'], help="backend timing model for DRAM")
parser.add_argument("--debug-memory", action="store_true", help="enable memory debug")
parser.add_argument("--debug-requests", action="store_true", help="enable debug of requests")
parser.add_argument("--debug-responses", action="store_true", help="enable debug of responses")
parser.add_argument("--debug-syscalls", action="store_true", help="enable debug of syscalls")
parser.add_argument("--verbose-memory", type=int, default=0, help="verbosity of memory")

arguments = parser.parse_args()

CORE_DEBUG['debug_memory'] = arguments.debug_memory
CORE_DEBUG['debug_requests'] = arguments.debug_requests
CORE_DEBUG['debug_responses'] = arguments.debug_responses
CORE_DEBUG['debug_syscalls'] = arguments.debug_syscalls

print("""
PANDOHammerDrvR:
  program = {}
""".format(
    arguments.program
))
class Tile(object):
    def l1sp_start(self):
        return (self.id+0)*4*1024

    def l1sp_end(self):
        return (self.id+1)*4*1024-1
    
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
        self.core = sst.Component("core_%d" % self.id, "Drv.RISCVCore")
        self.core.addParams({
            "verbose"   : arguments.verbose,
            "num_harts" : SYSCONFIG["sys_core_threads"],
            "clock"     : SYSCONFIG["sys_core_clock"],
            "program" : arguments.program,
            #"argv" : ' '.join(argv), @ todo, make this work
            "core": self.id,
            "pod" : self.pod,
            "pxn" : self.pxn,
        })
        self.core.addParams(SYSCONFIG)
        self.core.addParams(CORE_DEBUG)
        self.core_iface = self.core.setSubComponent("memory", "memHierarchy.standardInterface")
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
        self.initSP()

    def initSP(self):
        # set stack pointers
        stack_base = self.l1sp_start()
        stack_bytes = self.l1sp_end() - self.l1sp_start() + 1
        stack_words = stack_bytes // 8
        thread_stack_words = stack_words // SYSCONFIG["sys_core_threads"]
        thread_stack_bytes = thread_stack_words * 8
        # build a string of stack pointers
        # to pass a parameter to the core
        sp_v = ["{} {}".format(i, stack_base + ((i+1)*thread_stack_bytes) - 8) for i in range(SYSCONFIG["sys_core_threads"])]
        sp_str = "[" + ", ".join(sp_v) + "]"
        self.core.addParams({"sp" : sp_str})

    def __init__(self, id, pod=0, pxn=0):
        self.id = id
        self.pod = pod
        self.pxn = pxn
        """
        Create a tile with the given ID, pod, and PXN.
        """
        self.initCore()
        self.initMem()
        self.initRtr()

DRAM_BASE = 0x40000000
DRAM_SIZE = 0x40000000

class SharedMemory(object):
    def __init__(self, id):
        self.memctrl = sst.Component("memctrl_%d" % id, "memHierarchy.MemController")
        self.memctrl.addParams({
            "clock" : "1GHz",
            "addr_range_start" : DRAM_BASE+(id+0)*512*1024*1024+0,
            "addr_range_end"   : DRAM_BASE+(id+1)*512*1024*1024-1,
            "debug" : 1,
            "debug_level" : arguments.verbose_memory,
            "verbose" : arguments.verbose_memory,
        })
        # set the backend memory system to Drv special memory
        # (needed for AMOs)
        if (arguments.dram_backend == "simple"):
            self.memory = self.memctrl.setSubComponent("backend", "Drv.DrvSimpleMemBackend")
            self.memory.addParams({
                "verbose_level" : arguments.verbose_memory,
                "access_time" : "32ns",
                "mem_size" : "512MiB",
            })
        elif (arguments.dram_backend == "ramulator"):
            self.memory = self.memctrl.setSubComponent("backend", "memHierarchy.ramulator")
            self.memory.addParams({
                "verbose_level" : arguments.verbose_memory,
                "configFile" : "/root/sst-ramulator-src/configs/hbm4-pando-config.cfg",
                "mem_size" : "512MiB",
            })
        # set the custom command handler
        # we need to use the Drv custom command handler
        # to handle our custom commands (AMOs)
        self.customcmdhandler = self.memctrl.setSubComponent("customCmdHandler", "Drv.DrvCmdMemHandler")
        self.customcmdhandler.addParams({
            "verbose_level" : arguments.verbose_memory,
        })
        # network interface
        self.nic = self.memctrl.setSubComponent("cpulink", "memHierarchy.MemNIC")
        self.nic.addParams({
            "group" : 2,
            "network_bw" : "256GB/s",
            "verbose_level": arguments.verbose_memory,
        })
        
        # create the tile network
        self.mem_rtr = sst.Component("sharedmem_rtr_%d" % id, "merlin.hr_router")
        self.mem_rtr.addParams({
            # semantics parameters
            "id" : SHAREDMEM_RTR_ID+id,
            "num_ports" : 2,
            "topology" : "merlin.singlerouter",
            # performance models
            "xbar_bw" : "1024GB/s",
            "link_bw" : "1024GB/s",
            "flit_size" : "8B",
            "input_buf_size" : "1KB",
            "output_buf_size" : "1KB",
            "debug" : 1,
        })
        self.mem_rtr.setSubComponent("topology","merlin.singlerouter")

        # setup connection rtr <-> mem
        self.mem_nic_link = sst.Link("sharedmem_nic_link_%d" % id)
        self.mem_nic_link.connect(
            (self.nic, "port", "1ns"),
            (self.mem_rtr, "port0", "1ns"),
        )


# build the tiles
tiles = []
CORES = SYSCONFIG["sys_pod_cores"]
for i in range(CORES):
    tiles.append(Tile(i))

tiles[0].markAsLoader()

# build the shared memory
DRAM_PORTS = SYSCONFIG["sys_pod_dram_ports"]
dram_ports = []
for i in range(DRAM_PORTS):
    dram_ports.append(SharedMemory(i))
                      
# build the network crossbar
chiprtr = sst.Component("chiprtr", "merlin.hr_router")
chiprtr.addParams({
    # semantics parameters
    "id" : CHIPRTR_ID,
    "num_ports" : CORES+DRAM_PORTS,
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
base_dram_portno = len(tiles)
for (i, dram_port) in enumerate(dram_ports):
    bridge = sst.Component("bridge_%d" % (base_dram_portno+i), "merlin.Bridge")
    bridge.addParams({
        "translator" : "memHierarchy.MemNetBridge",
        "debug" : 1,
        "debug_level" : 10,
        "network_bw" : "256GB/s",
        })
    dram_bridge_link = sst.Link("dram_bridge_link_%d" % i)
    dram_bridge_link.connect(
        (bridge, "network0", "1ns"),
        (dram_port.mem_rtr, "port1", "1ns")
    )
    bridge_chiprtr_link = sst.Link("bridge_chip_memrtr_link_%d" %i)
    bridge_chiprtr_link.connect(
        (bridge, "network1", "1ns"),
        (chiprtr, "port%d" % (base_dram_portno+i), "1ns")
    )
        

