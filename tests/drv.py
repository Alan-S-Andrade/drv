# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington
import sst
import argparse
import addressmap

##################
# Size Constants #
##################
POD_L2SP_SIZE = (1<<25)
CORE_L1SP_SIZE = (1<<17)
        
################################
# parse command line arguments #
################################
parser = argparse.ArgumentParser()
parser.add_argument("program", help="program to run")
parser.add_argument("argv", nargs=argparse.REMAINDER, help="arguments to program")
parser.add_argument("--verbose", type=int, default=0, help="verbosity of core")
parser.add_argument("--dram-access-time", type=str, default="100ns", help="latency of DRAM (only valid if using the latency based model)")
parser.add_argument("--dram-backend", type=str, default="simple", choices=['simple', 'ramulator','dramsim3'], help="backend timing model for DRAM")
parser.add_argument("--dram-backend-config", type=str, default="/root/sst-ramulator-src/configs/hbm4-pando-config.cfg",
                    help="backend timing model configuration for DRAM")
parser.add_argument("--debug-init", action="store_true", help="enable debug of init")
parser.add_argument("--debug-memory", action="store_true", help="enable memory debug")
parser.add_argument("--debug-requests", action="store_true", help="enable debug of requests")
parser.add_argument("--debug-responses", action="store_true", help="enable debug of responses")
parser.add_argument("--debug-syscalls", action="store_true", help="enable debug of syscalls")
parser.add_argument("--debug-clock", action="store_true", help="enable debug of clock ticks")
parser.add_argument("--debug-mmio", action="store_true", help="enable debug of mmio requests to the core")
parser.add_argument("--verbose-memory", type=int, default=0, help="verbosity of memory")
parser.add_argument("--pod-cores", type=int, default=8, help="number of cores per pod")
parser.add_argument("--pxn-pods", type=int, default=1, help="number of pods")
parser.add_argument("--num-pxn", type=int, default=1, help="number of pxns")
parser.add_argument("--core-threads", type=int, default=16, help="number of threads per core")
parser.add_argument("--core-clock", type=str, default="1GHz", help="clock frequency of cores")
parser.add_argument("--core-max-idle", type=int, default=1, help="max idle time of cores")
parser.add_argument("--core-l1sp-size", type=int, default=CORE_L1SP_SIZE, help="size of l1sp per core")

parser.add_argument("--pod-l2sp-banks", type=int, default=8, help="number of l2sp banks per pod")
parser.add_argument("--pod-l2sp-interleave", type=int, default=0, help="interleave size of l2sp addresses (defaults to no  interleaving)")
parser.add_argument("--pod-l2sp-size", type=int, default=POD_L2SP_SIZE, help="size of l2sp per pod (max {} bytes)".format(POD_L2SP_SIZE))

parser.add_argument("--pxn-dram-banks", type=int, default=8, help="number of dram banks per pxn")
parser.add_argument("--pxn-dram-size", type=int, default=2*(1024**3), help="size of main memory per pxn (max {} bytes)".format(8*1024*1024*1024))
parser.add_argument("--pxn-dram-interleave", type=int, default=0, help="interleave size of dram addresses (defaults to no  interleaving)")

parser.add_argument("--with-command-processor", type=str, default="",
                    help="Command processor program to run. Defaults to empty string, in which no command processor will be included in the model.")
parser.add_argument("--cp-verbose", type=int, default=0, help="verbosity of command processor")
parser.add_argument("--cp-verbose-init", action="store_true", help="command processor enable debug of init")
parser.add_argument("--cp-verbose-requests", action="store_true", help="command processor enable debug of requests")
parser.add_argument("--cp-verbose-responses", action="store_true", help="command processor enable debug of responses")
parser.add_argument("--drvx-stack-in-l1sp", action="store_true", help="use l1sp backing storage as stack")
parser.add_argument("--drvr-isa-test", action="store_true", help="Running an ISA test")
parser.add_argument("--test-name", type=str, default="", help="Name of the test")
parser.add_argument("--core-stats", action="store_true", help="enable core statistics")
parser.add_argument("--stats-load-level", type=int, default=0, help="load level for statistics")
parser.add_argument("--trace-remote-pxn-memory", action="store_true", help="trace remote pxn memory accesses")

arguments = parser.parse_args()

#############################################
# Latencies of various components and links #
#############################################
# determine latency of link
LINK_LATENCIES = {
}
def link_latency(link_name):
    if link_name not in LINK_LATENCIES:
        return "1ps"

    return LINK_LATENCIES[link_name]

# determine the latency of a router
ROUTER_LATENCIES = {
    'tile_rtr': "250ps",
    'mem_rtr': "250ps",
    'chiprrtr': "250ps",
    'offchiprtr': "70ns",
}
def router_latency(router_name):
    if router_name not in ROUTER_LATENCIES:
        return "1ps"

    return ROUTER_LATENCIES[router_name]

# determine the latency of a memory
MEMORY_LATENCIES = {
    'l1sp': "1ns",
    'l2sp': "1ns",
    'dram': arguments.dram_access_time,
}
def memory_latency(memory_name):
    if memory_name not in MEMORY_LATENCIES:
        return "1ps"

    return MEMORY_LATENCIES[memory_name]


###################
# router id bases #
###################
COMPUTETILE_RTR_ID = 0
SHAREDMEM_RTR_ID = 1024
SHAREDMEM_CACHE_RTR_ID = 1024*1024
CHIPRTR_ID = 1024*1024*1024

SYSCONFIG = {
    "sys_num_pxn" : 1,
    "sys_pxn_pods" : 1,
    "sys_pod_cores" : 8,
    "sys_core_threads" : 16,
    "sys_core_clock" : "1GHz",
    "sys_core_l1sp_size" : arguments.core_l1sp_size,
    "sys_pxn_dram_size" : arguments.pxn_dram_size,    
    "sys_pxn_dram_ports" : arguments.pxn_dram_banks,
    "sys_pxn_dram_interleave_size" : arguments.pxn_dram_interleave if arguments.pxn_dram_interleave else arguments.pxn_dram_size//arguments.pxn_dram_banks,
    "sys_pod_l2sp_size" : arguments.pod_l2sp_size,
    "sys_pod_l2sp_banks" : arguments.pod_l2sp_banks,
    "sys_pod_l2sp_interleave_size" : arguments.pod_l2sp_interleave if arguments.pod_l2sp_interleave else arguments.pod_l2sp_size//arguments.pod_l2sp_banks,
    "sys_nw_flit_dwords" : 1,
    "sys_nw_obuf_dwords" : 8,
    "sys_cp_present" : False,
}

CORE_DEBUG = {
    "debug_init" : False,
    "debug_clock" : False,
    "debug_requests" : False,
    "debug_responses" : False,
    "debug_loopback" : False,
    "debug_mmio" : False,
    "debug_memory": False,
    "debug_syscalls" : False,
    "trace_remote_pxn" : False,
    "trace_remote_pxn_load" : False,
    "trace_remote_pxn_store" : False,
    "trace_remote_pxn_atomic" : False,
    "isa_test": False,
    "test_name": "",
}

KNOBS = {
    "core_max_idle" : arguments.core_max_idle,
}

SYSCONFIG['sys_num_pxn'] = arguments.num_pxn
SYSCONFIG['sys_pxn_pods'] = arguments.pxn_pods
SYSCONFIG['sys_pod_cores'] = arguments.pod_cores
SYSCONFIG['sys_core_threads'] = arguments.core_threads
SYSCONFIG['sys_cp_present'] = bool(arguments.with_command_processor)
SYSCONFIG['sys_core_clock'] = arguments.core_clock

CORE_DEBUG['debug_memory'] = arguments.debug_memory
CORE_DEBUG['debug_requests'] = arguments.debug_requests
CORE_DEBUG['debug_responses'] = arguments.debug_responses
CORE_DEBUG['debug_syscalls'] = arguments.debug_syscalls
CORE_DEBUG['debug_init'] = arguments.debug_init
CORE_DEBUG['debug_clock'] = arguments.debug_clock
CORE_DEBUG['debug_mmio'] = arguments.debug_mmio
CORE_DEBUG["trace_remote_pxn"] = arguments.trace_remote_pxn_memory
CORE_DEBUG['isa_test'] = arguments.drvr_isa_test
CORE_DEBUG['test_name'] = arguments.test_name


class Sysconfig(object):
    def __init__(self):
        """
        Initialize the system configuration
        """
        self._pxns = SYSCONFIG['sys_num_pxn']
        self._pxn_pods = SYSCONFIG['sys_pxn_pods']
        self._pod_cores = SYSCONFIG['sys_pod_cores']

    def pxns(self):
        """
        Return the number of PXNs in the system
        """
        return self._pxns

    def pods(self):
        """
        Return the number of pods in a pxn
        """
        return self._pxn_pods

    def cores(self):
        """
        Return the number of cores in a pod
        """
        return self._pod_cores

ADDRESS_MAP = addressmap.AddressMap(Sysconfig())

class L1SPRange(object):
    L1SP_SIZE = SYSCONFIG['sys_core_l1sp_size']
    L1SP_SIZE_STR = str(L1SP_SIZE) + 'B'
    def __init__(self, pxn, pod, core):
        builder = addressmap.L1SPAddressBuilder(ADDRESS_MAP, self.L1SP_SIZE)
        self.start, self.end, self.interleave_size, self.interleave_step = builder(pxn, pod, core)

class L2SPRange(object):
    # 16MiB
    L2SP_POD_BANKS = SYSCONFIG['sys_pod_l2sp_banks']
    L2SP_SIZE = SYSCONFIG['sys_pod_l2sp_size']
    L2SP_BANK_SIZE = L2SP_SIZE // L2SP_POD_BANKS
    L2SP_SIZE_STR = str(L2SP_SIZE) + 'B'
    L2SP_BANK_SIZE_STR = str(L2SP_BANK_SIZE) +'B'
    # interleave
    L2SP_INTERLEAVE_SIZE = SYSCONFIG['sys_pod_l2sp_interleave_size']
    L2SP_INTERLEAVE_SIZE_STR = "{}B".format(L2SP_INTERLEAVE_SIZE)
    L2SP_INTERLEAVE_STEP = L2SP_INTERLEAVE_SIZE * L2SP_POD_BANKS
    L2SP_INTERLEAVE_STEP_STR = "{}B".format(L2SP_INTERLEAVE_STEP)

    def __init__(self, pxn, pod, bank):
        builder = addressmap.L2SPAddressBuilder(ADDRESS_MAP, self.L2SP_BANK_SIZE, self.L2SP_INTERLEAVE_SIZE, self.L2SP_INTERLEAVE_STEP)
        self.start, self.end, self._interleave_size, self._interleave_step = builder(pxn, pod, bank)

    @property
    def interleave_size(self):
        return str(self._interleave_size) + 'B'

    @property
    def interleave_step(self):
        return str(self._interleave_step) + 'B'

    @property
    def bank_size(self):
        return self.L2SP_BANK_SIZE_STR

class MainMemoryRange(object):
    # specs say upto 8TB
    # we make this a parameter here
    MAINMEM_BANKS = SYSCONFIG['sys_pxn_dram_ports']
    MAINMEM_SIZE = SYSCONFIG['sys_pxn_dram_size']
    MAINMEM_BANK_SIZE = MAINMEM_SIZE // MAINMEM_BANKS
    # size strings
    MAINMEM_SIZE_STR = "{}GiB".format(MAINMEM_SIZE // 1024**3)
    MAINMEM_BANK_SIZE_STR = "{}MiB".format(MAINMEM_BANK_SIZE // 1024**2)
    # interleave
    MAINMEM_INTERLEAVE_SIZE = SYSCONFIG['sys_pxn_dram_interleave_size']
    MAINMEM_INTERLEAVE_SIZE_STR = "{}B".format(MAINMEM_INTERLEAVE_SIZE)
    MAINMEM_INTERLEAVE_STEP = MAINMEM_INTERLEAVE_SIZE * MAINMEM_BANKS
    MAINMEM_INTERLEAVE_STEP_STR = "{}B".format(MAINMEM_INTERLEAVE_STEP)

    def __init__(self, pxn, pod, bank):
        builder = addressmap.DRAMAddressBuilder(ADDRESS_MAP, self.MAINMEM_BANK_SIZE, self.MAINMEM_INTERLEAVE_SIZE, self.MAINMEM_INTERLEAVE_STEP)
        self.start, self.end, self._interleave_size, self._interleave_step = builder(pxn, bank)

    @property
    def interleave_size(self):
        return str(self._interleave_size) + 'B'

    @property
    def interleave_step(self):
        return str(self._interleave_step) + 'B'

    @property
    def bank_size(self):
        return self.MAINMEM_BANK_SIZE_STR


class CommandProcessor(object):
    CORE_ID = -1
    def initCore(self):
        """
        Initialize the tile's core
        """
        # create the core
        self.core = sst.Component("command_processor_pxn%d_pod%d" % (self.pxn, self.pod), "Drv.DrvCore")
        argv = []
        argv.append(arguments.program)
        argv.extend(arguments.argv)
        self.core.addParams({
            "verbose"   : arguments.verbose,
            "threads"   : 1,
            "clock"     : "2GHz",
            "executable": arguments.with_command_processor,
            "argv" : ' '.join(argv), # cp its own exe as first arg, then same argv as the main program
            "max_idle" : 100//8, # turn clock offf after idle for 1 us
            "id"  : self.id,
            "pod" : self.pod,
            "pxn" : self.pxn,
        })
        self.core.addParams(SYSCONFIG)
        self.core.addParams(CORE_DEBUG)

        self.core_mem = self.core.setSubComponent("memory", "Drv.DrvStdMemory")
        self.core_mem.addParams({
            "verbose" : arguments.cp_verbose,
            "verbose_init" : arguments.cp_verbose_init,
            "verbose_requests" : arguments.cp_verbose_requests,
            "verbose_responses" : arguments.cp_verbose_responses,
        })

        self.core_iface = self.core_mem.setSubComponent("memory", "memHierarchy.standardInterface")
        self.core_iface.addParams({
            "verbose" : arguments.cp_verbose,
        })
        self.core_nic = self.core_iface.setSubComponent("memlink", "memHierarchy.MemNIC")
        self.core_nic.addParams({
            "group" : 0,
            "network_bw" : "1024GB/s",
            "destinations" : "0,1,2",
            "verbose_level" : arguments.verbose_memory,
        })

    def __init__(self, pod=0, pxn=0):
        self.id = self.CORE_ID
        self.pod = pod
        self.pxn = pxn
        self.initCore()
