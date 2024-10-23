import sst
from addressmap import *

class MemoryBuilder(object):
    """
    A base class for a memory tile builder
    """
    def __init__(self):
        """
        Initialize the memory tile builder
        """
        super().__init__()
        return


    @property
    def group(self):
        """
        Return the routing group number
        """
        return 2

class Memory(object):
    """
    A base class for a memory tile
    """
    def __init__(self, name):
        """
        Initialize the memory tile
        """
        self.name = name
        return

    def network_interface(self):
        """
        Returns the network interface subcomponent and port name
        (interface, portname) pair
        """
        raise NotImplementedError("network_if() method not implemented")


class L1SP(Memory):
    """
    A base class for a L1 SP
    """
    def __init__(self, name):
        """
        Initialize the L1 SP memory tile
        """
        super().__init__(name)
        self.memctrl = None
        self.backend = None
        self.cmdhandler = None
        self.nic = None
        return

    def network_interface(self):
        return (self.nic, "port")

class L1SPBuilder(MemoryBuilder):
    """
    A base class for a L1 SP
    """
    def __init__(self):
        """
        Initialize the L1 SP memory tile builder
        """
        super().__init__()
        self.network_bw = "24GB/s"
        self.size = 4*1024
        self.clock = "1GHz"
        self.access_time = "1ns"
        return

    def memctrl_name(self, name):
        """
        Return the name of the memory controller
        """
        return name + "_memctrl"
    
    def build(self, system_builder, name):
        """
        Build the L1 SP memory tile
        """
        l1sp = L1SP(name)
        addrmap = system_builder.addressmap()
        addrrangebuilder = L1SPAddressBuilder(addrmap, self.size)
        addr_start, addr_stop, _0, _1 = addrrangebuilder(
            system_builder.pxn.id,
            system_builder.pxn.pod.id,
            system_builder.pxn.pod.compute.id,
        )
        # make the memory controller
        l1sp.memctrl = sst.Component(self.memctrl_name(name),\
                                     "memHierarchy.MemController")
        l1sp.memctrl.addParams({
            "clock" : self.clock,
            "addr_range_start" : addr_start,
            "addr_range_end" : addr_stop,
        })

        # make the backend
        l1sp.backend = l1sp.memctrl.setSubComponent("backend", "Drv.DrvSimpleMemBackend")
        l1sp.backend.addParams({
            "access_time" : self.access_time,
            "max_requests_per_cycle" : 1,
            "mem_size" : f'{self.size}B',
        })

        # make the command handler
        l1sp.cmdhandler = \
            l1sp.memctrl.setSubComponent("customCmdHandler", "Drv.DrvCmdMemHandler")
        
        # make the nic
        l1sp.nic = l1sp.memctrl.setSubComponent("cpulink", "memHierarchy.MemNIC")
        l1sp.nic.addParams({
            "group" : self.group,
            "network_bw" : self.network_bw,
        })
        return l1sp

class L2SP(Memory):
    """
    A base class for a L2SP
    """
    def __init__(self, name):
        """
        Initialize the L2 SP memory tile
        """
        super().__init__(name)
        self.memctrl = None
        self.backend = None
        self.cmdhandler = None
        self.nic = None
        return

    def network_interface(self):
        return (self.nic, "port")

class L2SPBuilder(MemoryBuilder):
    """
    A base class for a L2 SP memory tile builder
    """
    def __init__(self):
        """
        Initialize the L2 SP memory tile builder
        """
        self.id = 0
        self.size = 64*1024
        self.interleave_size = 0
        self.interleave_step = 0
        self.network_bw = "1GB/s"
        self.clock = "1GHz"
        return

    def memctrl_name(self, name):
        """
        Return the name of the memory controller
        """
        return name + "_memctrl"
    
    def build(self, system_builder, name):
        l2sp = L2SP(name)
        addrmap = system_builder.addressmap()
        addrrangebuilder = L2SPAddressBuilder(addrmap, \
                                              self.size, \
                                              self.interleave_size, \
                                              self.interleave_step)

        addr_start, addr_stop, addr_interleave_size, addr_interleave_step \
            = addrrangebuilder(system_builder.pxn.id, \
                               system_builder.pxn.pod.id, \
                               system_builder.pxn.pod.l2sp.id)

        l2sp.memctrl = sst.Component(self.memctrl_name(name),\
                                     "memHierarchy.MemController")
        l2sp.memctrl.addParams({
            "clock" : self.clock,
            "addr_range_start" : addr_start,
            "addr_range_end" : addr_stop,
            "interleave_size" : f'{addr_interleave_size}B',
            "interleave_step" : f'{addr_interleave_step}B'
        })

        l2sp.backend = l2sp.memctrl.setSubComponent("backend", "Drv.DrvSimpleMemBackend")
        l2sp.backend.addParams({
            "access_time" : self.access_time,
            "max_requests_per_cycle" : 1,
            "mem_size" : f'{self.size}B',
        })

        l2sp.cmdhandler = \
            l2sp.memctrl.setSubComponent("customCmdHandler", "Drv.DrvCmdMemHandler")

        l2sp.nic = l2sp.memctrl.setSubComponent("cpulink", "memHierarchy.MemNIC")
        l2sp.nic.addParams({
            "group" : self.group,
            "network_bw" : self.network_bw,
        })
        return l2sp

class DRAMBuilder(MemoryBuilder):
    """
    A base class for a DRAM memory tile builder
    """
    def __init__(self):
        """
        Initialize the DRAM memory tile builder
        """
        super().__init__()
        self.id = 0
        self.size = 1024*1024*1024
        self.interleave_size = 0
        self.interleave_step = 0
        self.network_bw = "1GB/s"
        self.clock = "1GHz"
        return

    def memctrl_name(self, name):
        """
        Return the name of the memory controller
        """
        return name + "_memctrl"

    def address_range(self, system_builder):
        """
        Return (start, stop, interleave_size, interleave_step)
        """
        addrmap = system_builder.addressmap()
        addrrangebuilder = DRAMAddressBuilder(addrmap, \
                                              self.size, \
                                              self.interleave_size, \
                                              self.interleave_step)
        return addrrangebuilder(system_builder.pxn.id, \
                                system_builder.pxn.dram.id)

    @property
    def memory_controller_model(self):
        if not self.is_coherent:
            return "memHierarchy.MemController"

        return "memHierarchy.CoherentMemController"

    @property
    def is_coherent(self):
        raise NotImplementedError

    def build_dram(self, system_builder, name):
        """
        Build the DRAM memory tile
        """
        dram = self.create_dram(name)
        addr_start, addr_stop, addr_interleave_size, addr_interleave_step \
            = self.address_range(system_builder)

        dram.memctrl = sst.Component(self.memctrl_name(name),\
                                     self.memory_controller_model)
        dram.memctrl.addParams({
            "clock" : self.clock,
            "addr_range_start" : addr_start,
            "addr_range_end" : addr_stop,
            "interleave_size" : f'{addr_interleave_size}B',
            "interleave_step" : f'{addr_interleave_step}B',
            "max_requests_per_cycle" : 1,
        })

        dram.backend = dram.memctrl.setSubComponent("backend", "Drv.DrvSimpleMemBackend")
        dram.backend.addParams({
            "access_time" : self.access_time,
            "mem_size" : f'{self.size}B',
            "max_requests_per_cycle" : 1,            
        })

        dram.cmdhandler = \
            dram.memctrl.setSubComponent("customCmdHandler", "Drv.DrvCmdMemHandler")
        dram.cmdhandler.addParams({
            "cache_line_size" : self.cache_line_size if self.is_coherent else 0,
            "shootdowns" : "true" if self.is_coherent else "false",
        })

        return dram

    def create_dram(self, name):
        """
        Create the DRAM memory tile
        """
        raise NotImplementedError("create_dram() is not implemented")


    def build(self, system_builder, name):
        raise NotImplementedError("build() is not implemented")

class NoCacheDRAM(Memory):
    """
    A base class for a DRAM
    """
    def __init__(self, name):
        """
        Initialize the DRAM memory tile
        """
        super().__init__(name)
        self.memctrl = None
        self.backend = None
        self.cmdhandler = None
        self.nic = None
        return

    def network_interface(self):
        return (self.nic, "port")

class NoCacheDRAMBuilder(DRAMBuilder):
    """
    A base class for a DRAM memory tile builder
    """
    def __init__(self):
        """
        Initialize the DRAM memory tile builder
        """
        super().__init__()
        return

    def create_dram(self, name):
        """
        Create the DRAM memory tile
        """
        return NoCacheDRAM(name)

    @property
    def is_coherent(self):
        return False

    def build(self, system_builder, name):
        dram = self.build_dram(system_builder, name)
        # create the network interface
        dram.nic = dram.memctrl.setSubComponent("cpulink", "memHierarchy.MemNIC")
        dram.nic.addParams({
            "group" : self.group,
            "network_bw" : self.network_bw,
        })
        return dram

class CachedDRAM(Memory):
    """
    A base class for a DRAM bank with a cache in front of it
    """
    def __init__(self, name):
        """
        Initialize the cached DRAM memory tile
        """
        super().__init__(name)
        self.memctrl = None
        self.backend = None
        self.cmdhandler = None
        self.cache = None
        self.mem_cpulink = None # nic going to the memory
        self.cache_memlink = None # cache nic going to the memory
        self.cache_cpunic = None # cache nic going to the cpu
        return

    def network_interface(self):
        return (self.cache_cpunic, "port")

class CachedDRAMBuilder(DRAMBuilder):
    """
    Builds a DRAM bank with a cache in front of it
    """
    def __init__(self):
        """
        Initialize the cached DRAM memory tile builder
        """
        super().__init__()
        self.id = 0
        self.size = 1024*1024*1024
        self.interleave_size = 0
        self.interleave_step = 0
        self.network_bw = "1GB/s"
        self.cache_size = 64*1024
        self.cache_assoc = 8
        self.cache_line_size = 64
        self.clock = "1GHz"
        self.mshr_num_entries = 16
        return

    def cache_name(self, name):
        """
        Return the name of the cache
        """
        return name + "_cache"

    @property
    def sources(self):
        return "0,1"

    @property
    def is_coherent(self):
        return True

    def create_dram(self, name):
        """
        Create the DRAM memory tile
        """
        return CachedDRAM(name)

    def build(self, system_builder, name):
        dram = self.build_dram(system_builder, name)

        addr_start, addr_stop, addr_interleave_size, addr_interleave_step \
            = self.address_range(system_builder)

        # create the cache
        dram.cache = sst.Component(self.cache_name(name), "memHierarchy.Cache")
        dram.cache.addParams({
            "cache_frequency" : self.clock,
            # cache size, associativity, replacement policy, etc.
            "cache_size" : f'{self.cache_size}B',
            "associativity" : self.cache_assoc,
            "cache_line_size" : self.cache_line_size,
            "mshr_num_entries" : self.mshr_num_entries,
            "replacement_policy" : "lru",
            "access_latency_cycles" : 1,
            # routing information
            "addr_range_start" : addr_start,
            "addr_range_end" : addr_stop,
            "interleave_size" : f'{addr_interleave_size}B',
            "interleave_step" : f'{addr_interleave_step}B',
            # required for this to work; don't change
            "L1" : "true",
            "coherence_protocol" : "mesi",
            "cache_type" : "inclusive"
        })

        # connect the cache to the memory
        dram.mem_cpulink = dram.memctrl.setSubComponent("cpulink", \
                                                        "memHierarchy.MemLink")
        dram.cache_memlink = dram.cache.setSubComponent("memlink", \
                                                        "memHierarchy.MemLink")
        link = sst.Link(f"link_{self.cache_name(name)}_to_{self.memctrl_name(name)}")
        link.connect((dram.cache_memlink, "port", "1ns"), \
                     (dram.mem_cpulink, "port", "1ns"))

        # create the NIC for the memory
        dram.cache_cpunic = dram.cache.setSubComponent("cpulink", "memHierarchy.MemNIC")
        dram.cache_cpunic.addParams({
            "group" : self.group,
            "sources" : self.sources,
            "network_bw" : self.network_bw,
        })

        return dram
