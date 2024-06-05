from drv import *

cacheless = False

sys = {
    'sys_pod_cores' : 1,
    'sys_pxn_dram_ports' : 1,
}

latency = {
    "l1sp" : "1ns",
    "l2sp" : "1us",
    "dram" : "1ms",
}

GROUPS = {
    "core" : "0",
    "cache" : "1",
    "memory" : "2",
}

group = {
    "core"   : GROUPS["core"],
    "l1sp"   : GROUPS["memory"],
    "l2sp"   : GROUPS["memory"],
    "vcache" : GROUPS["cache"],
    "dram"   : GROUPS["memory"],
}

destinations = {
    "core" : ",".join([GROUPS["cache"],GROUPS["memory"]]),
    "l1sp" : "",
    "l2sp" : "",
    "vcache" : GROUPS["memory"],
    "dram" : GROUPS["cache"],
}

sources = {
    "core" : "",
    "l1sp" : GROUPS["core"],
    "l2sp" : GROUPS["core"],
    "dram" : ",".join([GROUPS["cache"],GROUPS["core"]]),
    "vcache" : ",".join([GROUPS["core"],GROUPS["memory"]])
}

nic_debug = {
    "debug" : 0,
    "debug_level" : 10,
}

dram_memctrl_debug = {
    "debug" : 0,
    "debug_level" : 10,
}

dram_cachectrl_debug = {
    "debug" : 0,
    "debug_level" : 10,
}

class Endpoint(object):
    def network_if(self):
        """
        Return the network interface component, and port name
        """
        raise NotImplementedError
    
class ComputeTile(Endpoint):
    def __init__(self, y, x):
        """
        Build core and l1sp
        """
        super().__init__()        
        self.x = x
        self.y = y
        pxn, pod, x, y = self.id()
        self.l1sprange = L1SPRange(pxn, pod, y, x)
        self.core = self.make_core()
        self.l1sp = self.make_l1sp()
        self.bridge = self.make_bridge()
        self.router = self.make_router()
        # connect
        sst.Link("core_nic_link_{}".format(self.name())).connect(
            (self.core_endpoint(), "port", "1ps"),
            (self.router, "port0", "1ps")
        )
        sst.Link("l1sp_nic_link_{}".format(self.name())).connect(
            (self.l1sp_endpoint(), "port", "1ps"),
            (self.router, "port1", "1ps")
        )
        sst.Link("router_bridge_link_{}".format(self.name())).connect(
            (self.router, "port2", "1ps"),
            (self.bridge, "network0", "1ps")
        )
        
        
    def core_endpoint(self):
        """
        Return the core NIC
        """
        mem = self.core.setSubComponent("memory", "Drv.DrvStdMemory")
        iface = mem.setSubComponent("memory", "memHierarchy.standardInterface")        
        nic = iface.setSubComponent("memlink", "memHierarchy.MemNIC")
        nic.addParams({
            "group" : group["core"],
            "network_bw" : "1GB/s",
            "sources" : sources["core"],
            "destinations" : destinations["core"],
        })
        nic.addParams(nic_debug)                
        return nic

    def l1sp_endpoint(self):
        """
        Return the L1SP NIC
        """
        nic = self.l1sp.setSubComponent("cpulink", "memHierarchy.MemNIC")
        nic.addParams({
            "group" : group["l1sp"],
            "network_bw" : "1GB/s",
            "sources" : sources["l1sp"],
            "destinations" : destinations["l1sp"],
        })
        nic.addParams(nic_debug)
        return nic

    def make_bridge(self):
        """
        Return the bridge NIC
        """
        bridge = sst.Component("bridge_" + self.name(), "merlin.Bridge")
        bridge.addParams({
            "translator" : "memHierarchy.MemNetBridge",
            "network_bw" : "1GB/s",
        })
        return bridge
        

    def id(self):
        """
        Return the PXN, POD, X, and Y coordinates of this tile
        """
        return 0, 0, self.x, self.y

    def name(self):
        """
        Return the name of this tile
        """
        pxn, pod, x, y = self.id()
        #return "pxn{}_pod{}_x{}_y{}".format(pxn, pod, x, y)
        return ""
    
    def make_core(self):
        pxn, pod, x, y = self.id()
        #core_name = 'core_' + self.name()
        core_name = 'core'
        core = sst.Component(core_name,"Drv.DrvCore")
        core.addParams({
            "verbose"   : arguments.verbose,
            "threads"   : 1,
            "executable": arguments.program,
            "args"      : ' '.join(arguments.argv),
            "max_idle"  : KNOBS["core_max_idle"],
            "id" : (y << 3) | x,
            "pod": pod,
            "pxn": pxn,
            "stack_in_l1sp": arguments.drvx_stack_in_l1sp,
        })
        # system paramaters
        core.addParams(sys)
        return core

    def make_l1sp(self):
        #l1sp_name = 'l1sp_' + self.name()
        l1sp_name = 'l1sp'
        l1sp = sst.Component(l1sp_name, "memHierarchy.MemController")
        l1sp.addParams({
            "debug" : 0,
            "debug_level" : 0,
            "clock" : "1GHz",
            "addr_range_start" : self.l1sprange.start,
            "addr_range_end" : self.l1sprange.end,
        })
        backend = l1sp.setSubComponent("backend", "Drv.DrvSimpleMemBackend")
        backend.addParams({
            "verbose_level" : arguments.verbose_memory,
            "access_time"   : latency["l1sp"],
            "mem_size" : L1SPRange.L1SP_SIZE_STR,
        })
        customcmdhandler = l1sp.setSubComponent("customCmdHandler", "Drv.DrvCmdMemHandler")
        customcmdhandler.addParams({
            "verbose_level" : arguments.verbose_memory,
        })
        return l1sp


    def make_router(self):
        router_name = 'rtr_' + self.name()
        rtr = sst.Component(router_name, "merlin.hr_router")
        rtr.addParams({
            "id"              : "0",
            "num_ports"       : 3,
            "xbar_bw"         : "1GB/s",
            "link_bw"         : "1GB/s",
            "flit_size"       : "8B",
            "input_buf_size"  : "1KB",
            "output_buf_size" : "1KB",
            "input_latency"   : "1ps",
            "output_latency"  : "1ps",
        })
        rtr.setSubComponent("topology", "merlin.singlerouter")
        return rtr

    def network_if(self):
        """
        Return the network interface
        """
        return (self.bridge, "network1")
    
class DRAMTile(Endpoint):
    def __init__(self):
        """
        Build memory and set backend
        """
        super().__init__()
        pxn, bank = self.id()
        self.dramrange = MainMemoryRange(pxn, pod=0, bank=bank)
        self.cache_line_size = 64
        # create the memory
        self.memory = self.make_memory()
        # create the cache
        self.cache = self.make_cache()
        # connect the cache to the memory
        mem_port = self.memory.setSubComponent("cpulink", "memHierarchy.MemLink")
        cache_port = self.cache.setSubComponent("memlink", "memHierarchy.MemLink")
        link = sst.Link("vcache2mem")
        link.connect((cache_port, "port", "1ns"), (mem_port, "port", "1ns"))
        
    def make_memory(self):
        # create the memory
        memory = sst.Component("dram", "memHierarchy.CoherentMemController")        
        memory.addParams({
            "clock" : "1GHz",
            "addr_range_start" : self.dramrange.start,
            "addr_range_end"   : self.dramrange.end,
            "interleave_size"  : self.dramrange.interleave_size,
            "interleave_step"  : self.dramrange.interleave_step,
            "debug" : 0,
            "debug_level" : 0,
            "verbose" : arguments.verbose_memory,
        })
        memory.addParams(dram_memctrl_debug)
        backend = memory.setSubComponent("backend", "Drv.DrvSimpleMemBackend")
        backend.addParams({
            "verbose_level" : arguments.verbose_memory,
            "access_time"   : latency["dram"],
            "mem_size" : MainMemoryRange.MAINMEM_BANK_SIZE_STR,
        })
        customcmdhandler = memory.setSubComponent("customCmdHandler", "Drv.DrvCmdMemHandler")
        customcmdhandler.addParams({
            "verbose_level" : arguments.verbose_memory,
            "cache_line_size" : self.cache_line_size,
            "shootdowns" : True,
        })
        return memory

    def make_cache(self):
        cache = sst.Component("vcache", "memHierarchy.Cache")
        cache.addParams({
            "cache_frequency" : "1GHz",
            "cache_size" : "1MiB",
            "L1" : "true",
            "coherence_protocol" : "mesi",
            "cache_type" : "inclusive",
            "replacement_policy" : "lru",
            "access_latency_cycles" : 1,            
            "associativity" : 2,
            "cache_line_size" : self.cache_line_size,
            "mshr_num_entries" : 2,
            "addr_range_start" : self.dramrange.start,
            "addr_range_end" : self.dramrange.end,
            "interleave_size" : self.dramrange.interleave_size,
            "interleave_step" : self.dramrange.interleave_step,
        })
        cache.addParams(dram_cachectrl_debug)
        return cache
    
    def id(self):
        """
        Return pxn and bank id
        """
        return 0, 0
    
    def network_if(self):
        """
        Return the NIC component for this tile
        """
        if not hasattr(self, "nic"):
            self.nic = self.cache.setSubComponent("cpulink", "memHierarchy.MemNIC")
            self.nic.addParams({
                "group"        : group["vcache"],
                "network_bw"   : "1GB/s",
                "sources"      : sources["vcache"],
                "destinations" : destinations["vcache"],
            })
            self.nic.addParams(nic_debug)
        return (self.nic, "port")
    

class CachelessDRAMTile(Endpoint):
    def __init__(self):
        """
        Build memory and set backend
        """
        super().__init__()
        pxn, bank = self.id()
        self.dramrange = MainMemoryRange(pxn, pod=0, bank=bank)

        # create the memory
        self.memory = self.make_memory()        
        
    def make_memory(self):
        # create the memory
        memory = sst.Component("dram", "memHierarchy.MemController")        
        memory.addParams({
            "clock" : "1GHz",
            "addr_range_start" : self.dramrange.start,
            "addr_range_end"   : self.dramrange.end,
            "interleave_size"  : str(self.dramrange.interleave_size) + 'B',
            "interleave_step"  : str(self.dramrange.interleave_step) + 'B',
            "debug" : 0,
            "debug_level" : 0,
            "verbose" : arguments.verbose_memory,
        })
        backend = memory.setSubComponent("backend", "Drv.DrvSimpleMemBackend")
        backend.addParams({
            "verbose_level" : arguments.verbose_memory,
            "access_time"   : latency["dram"],
            "mem_size" : MainMemoryRange.MAINMEM_BANK_SIZE_STR,
        })
        customcmdhandler = memory.setSubComponent("customCmdHandler", "Drv.DrvCmdMemHandler")
        customcmdhandler.addParams({
            "verbose_level" : arguments.verbose_memory,
        })
        return memory
    
    def id(self):
        """
        Return pxn and bank id
        """
        return 0, 0
    
    def network_if(self):
        """
        Return the NIC component for this tile
        """
        if not hasattr(self, "nic"):
            self.nic = self.memory.setSubComponent("cpulink", "memHierarchy.MemNIC")
            self.nic.addParams({
                "group"        : group["dram"],
                "network_bw"   : "1GB/s",
                "sources"      : sources["dram"],
                "destinations" : destinations["dram"],
            })
            self.nic.addParams(nic_debug)
        return (self.nic, "port")
    
class L2SPTile(Endpoint):
    def __init__(self):
        """
        Build l2sp memory and set backend
        """
        super().__init__()
        pxn, pod, bank = self.id()
        self.l2sprange = L2SPRange(pxn, pod, bank)
        #print ("L2SPRange: [{:x},{:x}]".format(self.l2sprange.start, self.l2sprange.end))
        self.memory = sst.Component("l2sp", "memHierarchy.MemController")
        self.memory.addParams({
            "debug" : 0,
            "debug_level" : 0,
            "clock" : "1GHz",
            "addr_range_start" : self.l2sprange.start,
            "addr_range_end" : self.l2sprange.end,
        })
        backend = self.memory.setSubComponent("backend", "Drv.DrvSimpleMemBackend")
        backend.addParams({
            "verbose_level" : arguments.verbose_memory,
            "access_time"   : latency["l2sp"],
            "mem_size" : L2SPRange.L2SP_SIZE_STR,
        })
        customcmdhandler = self.memory.setSubComponent("customCmdHandler", "Drv.DrvCmdMemHandler")
        customcmdhandler.addParams({
            "verbose_level" : arguments.verbose_memory,
        })

    def id(self):
        """
        Return pxn, pod, and bank id
        """
        return 0, 0, 0

    def network_if(self):
        """
        Return the NIC component for this tile
        """
        if not hasattr(self, "nic"):
            self.nic = self.memory.setSubComponent("cpulink", "memHierarchy.MemNIC")
            self.nic.addParams({
                "group"        : group["l2sp"],
                "network_bw"   : "1GB/s",
                "sources"      : sources["l2sp"],
                "destinations" : destinations["l2sp"],
            })
            self.nic.addParams(nic_debug)
        return (self.nic, "port")

class Network(object):
    def __init__(self):
        """
        Initialize lists of computre, dram, and l2sp tiles
        """
        self.compute_tiles = []
        self.dram_tiles = []
        self.l2sp_tiles = []
        self.router = sst.Component("router", "merlin.hr_router")

    def add_compute(self, comp):
        self.compute_tiles.append(comp)

    def add_dram(self, dram):
        self.dram_tiles.append(dram)

    def add_l2sp(self, l2sp):
        self.l2sp_tiles.append(l2sp)

    def ports(self):
        """
        Return the number of ports in the network
        """
        return 2*len(self.compute_tiles) + len(self.dram_tiles) + len(self.l2sp_tiles)

    def endpoints(self):
        """
        Iterate over all endpoints nics in the network
        """
        for comp in self.compute_tiles:
            # return the nic for the core
            yield comp.network_if()

        for l2sp in self.l2sp_tiles:
            # return the nic for the l2sp
            yield l2sp.network_if()

        for dram in self.dram_tiles:
            # return the nic for the dram
            yield dram.network_if()

    def id(self):
        """
        Return the network id
        """
        return 0

    def build(self):
        """
        Construct the network after all endpoints have been added
        """
        self.router.addParams({
            "id" : self.id(),
            "num_ports" : self.ports(),
            "xbar_bw" : "1GB/s",
            "link_bw" : "1GB/s",
            "flit_size" : "8B",
            "input_buf_size" : "1KB",
            "output_buf_size" : "1KB",
            "input_latency" : "1ps",
            "output_latency" : "1ps",
        })
        self.router.setSubComponent("topology", "merlin.singlerouter")

        for portno, network_if in enumerate(self.endpoints()):
            nic, portname = network_if
            link = sst.Link("network_link_%d" % portno)
            link.connect(
                (self.router, "port%d"%portno, "1ps"),
                (nic, portname, "1ps"),
            )


network = Network()
network.add_compute(ComputeTile(0,0))
#network.add_compute(ComputeTile(0,1))
network.add_l2sp(L2SPTile())
if sys['sys_pxn_dram_ports']:
    if cacheless:
        network.add_dram(CachelessDRAMTile())
    else:
        network.add_dram(DRAMTile())
network.build()
