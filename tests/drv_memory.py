# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington
import sst
from drv import *


class MemoryBank(object):
    """
    Base class for memory banks
    """
    def __init__(self, *args, **kwargs):
        return

class L2SPBank(MemoryBank):
    """
    A bank of L2 scratchpad memory
    """
    def __init__(self, pxn, pod, bank, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.pxn = pxn
        self.pod = pod
        self.bank = bank
        self.address_range = L2SPRange(self.pxn, self.pod, self.bank)
        self.make_memory()
        self.make_network_interface()

    def make_memory(self):
        """
        Create the memory controller
        """
        memory = sst.Component("{}_memctrl".format(self.name()), "memHierarchy.MemController")
        memory.addParams({
            "clock" : "1GHz",
            "addr_range_start" : self.address_range.start,
            "addr_range_end" : self.address_range.end,
            "interleave_size" : self.address_range.interleave_size,
            "interleave_step" : self.address_range.interleave_step,
        })
        self.memory = memory
        self.make_backend()
        self.make_cmdhandler()

    def make_backend(self):
        """
        Create the backend timing model for the memory
        """
        backend = self.memory.setSubComponent("backend", "Drv.DrvSimpleMemBackend")
        backend.addParams({
            "verbose_level" : arguments.verbose_memory,
            "access_time" : memory_latency('l2sp'),
            "mem_size" : self.address_range.bank_size,
        })
        self.memory_backend = backend

    def make_cmdhandler(self):
        """
        Create the custom command handler for the memory
        """
        cmdhandler = self.memory.setSubComponent("customCmdHandler", "Drv.DrvCmdMemHandler")
        cmdhandler.addParams({
            "verbose_level" : arguments.verbose_memory,
        })
        self.memory_cmdhandler = cmdhandler

    def make_network_interface(self):
        """
        Create the network interface for the memory
        """
        nic = self.memory.setSubComponent("cpulink", "memHierarchy.MemNIC")
        nic.addParams({
            "group" : 2,
            "sources" : "0",
            "network_bw" : "1024GB/s",
        })
        self.nwif = nic

    def name(self):
        """
        Return the name of the memory bank
        """
        return "l2sp_pxn{}_pod{}_bank{}".format(self.pxn, self.pod, self.bank)

    def network_if(self):
        """
        Return the network interface component, and port name
        """
        return (self.nwif, "port")

class DRAMBankBase(MemoryBank):
    """
    Base class for DRAM banks
    """
    def __init__(self, pxn, bank, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.pxn = pxn
        self.bank = bank
        self.address_range = MainMemoryRange(self.pxn, pod=0, bank=self.bank)
        self.cache_line_size = 64
        self.make_memory()

    def make_memory(self):
        """
        Create the memory controller
        """
        memory = sst.Component("{}_memctrl".format(self.name()), self.memory_controller_type)
        memory.addParams({
            "clock" : "1GHz",
            "addr_range_start" : self.address_range.start,
            "addr_range_end" : self.address_range.end,
            "interleave_size" : self.address_range.interleave_size,
            "interleave_step" : self.address_range.interleave_step,
            "debug" : 0,
            "debug_level" : 0,
        })
        self.memory = memory
        self.make_cmdhandler()
        self.make_backend()

    def make_backend(self):
        """
        Create the backend timing model for the memory
        """
        if (arguments.dram_backend == "ramulator"):
            backend = self.memory.setSubComponent("backend", "Drv.DrvRamulatorBackend")
            backend.addParams({
                "configFile" : arguments.dram_backend_config,
                "mem_size" : self.address_range.bank_size,
            })
        elif (arguments.dram_backend == "dramsim3"):
            backend = self.memory.setSubComponent("backend", "memHierarchy.dramsim3")
            backend.addParams({
                "config_ini" : arguments.dram_backend_config,
                "mem_size" : self.address_range.bank_size,
            })
        else:
            backend = self.memory.setSubComponent("backend", "Drv.DrvSimpleMemBackend")
            backend.addParams({
                "access_time" : memory_latency("dram"),
                "mem_size" : self.address_range.bank_size,
            })
        self.memory_backend = backend

    def make_cmdhandler(self):
        """
        Create the command handler for the memory
        """
        cmdhandler = self.memory.setSubComponent("customCmdHandler", "Drv.DrvCmdMemHandler")
        cmdhandler.addParams({
            "cache_line_size" : self.cache_line_size if self.is_coherent else 0,
            "shootdowns" : "true" if self.is_coherent else "false",
        })
        self.memory_cmdhandler = cmdhandler


    @property
    def memory_controller_type(self):
        """
        Return the type of memory controller
        """
        if self.is_coherent:
            return "memHierarchy.CoherentMemController"
        else:
            return "memHierarchy.MemController"

    @property
    def is_coherent(self):
        """
        Return true if this bank is coherent
        """
        raise NotImplementedError



class CachedDRAMBank(DRAMBankBase):
    """
    Cached DRAM bank
    """
    def __init__(self, pxn, bank, *args, **kwargs):
        self.cache_line_size = 64
        super().__init__(pxn, bank, *args, **kwargs)
        self.make_cache()
        self.connect_cache_to_memory()
        self.make_network_interface()

    def make_cache(self):
        """
        Create the cache controller
        """
        cache = sst.Component("{}_cache".format(self.name()), "memHierarchy.Cache")
        cache.addParams({
            "cache_frequency" : "1GHz",
            # cache size and configuration
            "cache_size" : "1MiB",
            "associativity" : 2,
            "cache_line_size" : self.cache_line_size,
            "mshr_num_entries" : 2,
            "replacement_policy" : "lru",
            "access_latency_cycles" : 1,
            # routing information
            "addr_range_start" : self.address_range.start,
            "addr_range_end" : self.address_range.end,
            "interleave_size" : self.address_range.interleave_size,
            "interleave_step" : self.address_range.interleave_step,
            # required for this to work at all don't change
            "L1" : "true",
            "coherence_protocol" : "mesi",
            "cache_type" : "inclusive",
        })
        self.cache = cache

    def connect_cache_to_memory(self):
        """
        Connect the cache controller to the memory controller
        """
        mem_port = self.memory.setSubComponent("cpulink", "memHierarchy.MemLink")
        cache_port = self.cache.setSubComponent("memlink", "memHierarchy.MemLink")
        link = sst.Link("{}_cache_to_mem".format(self.name()))
        link.connect(
            (cache_port, "port", "1ns"),
            (mem_port, "port", "1ns")
        )
        self.memory_port = mem_port
        self.cache_port = cache_port
        self.cache_to_mem = link

    def make_network_interface(self):
        """
        Create the network interface
        """
        nwif = self.cache.setSubComponent("cpulink", "memHierarchy.MemNIC")
        nwif.addParams({
            "group" : 2,
            "sources" : "0,1",
            "network_bw" : "256GB/s",
        })
        self.nwif = nwif

    def name(self):
        """
        Returns the name of the bank
        """
        return "cached_dram_pxn{}_bank{}".format(self.pxn, self.bank)

    def network_if(self):
        """
        Returns (interface, portname) pair
        Outputs should be connected to a merlin router
        """
        return (self.nwif, "port")

    @property
    def is_coherent(self):
        """
        Returns whether the bank is coherent
        """
        return True

class UncachedDRAMBank(DRAMBankBase):
    """
    Uncached DRAM bank
    """
    def __init__(self, pxn, bank, *args, **kwargs):
        super().__init__(pxn, bank, *args, **kwargs)
        self.make_network_interface()

    def make_network_interface(self):
        """
        Create the network interface
        """
        nwif = self.memory.setSubComponent("cpulink", "memHierarchy.MemNIC")
        nwif.addParams({
            "group" : 2,
            "network_bw" : "256GB/s",
        })
        self.nwif = nwif

    def name(self):
        """
        Returns the name of the bank
        """
        return "uncached_dram_pxn{}_bank{}".format(self.pxn, self.bank)

    def network_if(self):
        """
        Returns (interface, portname) pair
        Outputs should be connected to a merlin router
        """
        return (self.nwif, "port")

    @property
    def is_coherent(self):
        """
        Returns whether the bank is coherent
        """
        return False
