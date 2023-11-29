# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington
import sst
import argparse


# common functions
ADDR_TYPE_HI,ADDR_TYPE_LO     = (63, 58)
ADDR_PXN_HI, ADDR_PXN_LO      = (57, 44)
ADDR_POD_HI,ADDR_POD_LO       = (39, 34)
ADDR_CORE_Y_HI,ADDR_CORE_Y_LO = (30, 28)
ADDR_CORE_X_HI,ADDR_CORE_X_LO = (24, 22)

ADDR_TYPE_L1SP    = 0b000000
ADDR_TYPE_L2SP    = 0b000001
ADDR_TYPE_MAINMEM = 0b000100

def set_bits(word, hi, lo, value):
    mask = (1 << (hi - lo + 1)) - 1
    word &= ~(mask << lo)
    word |= (value & mask) << lo
    return word

class L1SPRange(object):
    L1SP_SIZE = 0x0000000000020000
    L1SP_SIZE_STR = "128KiB"
    def __init__(self, pxn, pod, core_y, core_x):
        start = 0
        start = set_bits(start, ADDR_TYPE_HI, ADDR_TYPE_LO, ADDR_TYPE_L1SP)
        start = set_bits(start, ADDR_PXN_HI, ADDR_PXN_LO, pxn)
        start = set_bits(start, ADDR_POD_HI, ADDR_POD_LO, pod)
        start = set_bits(start, ADDR_CORE_Y_HI, ADDR_CORE_Y_LO, core_y)
        start = set_bits(start, ADDR_CORE_X_HI, ADDR_CORE_X_LO, core_x)
        self.start = start
        self.end = start + self.L1SP_SIZE - 1

class L2SPRange(object):
    # 16MiB
    L2SP_POD_BANKS = 4
    L2SP_SIZE = 0x0000000001000000
    L2SP_BANK_SIZE = L2SP_SIZE // L2SP_POD_BANKS
    L2SP_SIZE_STR = "16MiB"
    L2SP_BANK_SIZE_STR = "4MiB"
    def __init__(self, pxn, pod, bank):
        start = 0
        start = set_bits(start, ADDR_TYPE_HI, ADDR_TYPE_LO, ADDR_TYPE_L2SP)
        start = set_bits(start, ADDR_PXN_HI, ADDR_PXN_LO, pxn)
        start = set_bits(start, ADDR_POD_HI, ADDR_POD_LO, pod)
        self.start = start + bank * self.L2SP_BANK_SIZE
        self.end = self.start + self.L2SP_BANK_SIZE - 1
        
class MainMemoryRange(object):
    # spec says upto 8TB
    # for simulation, we'll use 1GB/pod
    POD_MAINMEM_BANKS = 8
    POD_MAINMEM_SIZE = 0x0000000040000000
    POD_MAINMEM_BANK_SIZE = POD_MAINMEM_SIZE // POD_MAINMEM_BANKS
    POD_MAINMEM_SIZE_STR = "1GB"
    POD_MAINMEM_BANK_SIZE_STR = "128MB"
    def __init__(self, pxn, pod, bank):
        start = 0
        start = set_bits(start, ADDR_TYPE_HI, ADDR_TYPE_LO, ADDR_TYPE_MAINMEM)
        start = set_bits(start, ADDR_PXN_HI, ADDR_PXN_LO, pxn)
        self.start = start \
                   + pod  * self.POD_MAINMEM_SIZE \
                   + bank * self.POD_MAINMEM_BANK_SIZE
        self.end = self.start + self.POD_MAINMEM_BANK_SIZE - 1

        
################################
# parse command line arguments #
################################
parser = argparse.ArgumentParser()
parser.add_argument("program", help="program to run")
parser.add_argument("argv", nargs=argparse.REMAINDER, help="arguments to program")
parser.add_argument("--verbose", type=int, default=0, help="verbosity of core")
parser.add_argument("--dram-backend", type=str, default="simple", choices=['simple', 'ramulator'], help="backend timing model for DRAM")
parser.add_argument("--dram-backend-config", type=str, default="/root/sst-ramulator-src/configs/hbm4-pando-config.cfg",
                    help="backend timing model configuration for DRAM")
parser.add_argument("--debug-init", action="store_true", help="enable debug of init")
parser.add_argument("--debug-memory", action="store_true", help="enable memory debug")
parser.add_argument("--debug-requests", action="store_true", help="enable debug of requests")
parser.add_argument("--debug-responses", action="store_true", help="enable debug of responses")
parser.add_argument("--debug-syscalls", action="store_true", help="enable debug of syscalls")
parser.add_argument("--debug-clock", action="store_true", help="enable debug of clock ticks")
parser.add_argument("--verbose-memory", type=int, default=0, help="verbosity of memory")
parser.add_argument("--pod-cores", type=int, default=8, help="number of cores per pod")
parser.add_argument("--pxn-pods", type=int, default=1, help="number of pods")
parser.add_argument("--num-pxn", type=int, default=1, help="number of pxns")
parser.add_argument("--core-threads", type=int, default=16, help="number of threads per core")
parser.add_argument("--with-command-processor", type=str, default="",
                    help="Command processor program to run. Defaults to empty string, in which no command processor will be included in the model.")
parser.add_argument("--cp-verbose", type=int, default=0, help="verbosity of command processor")
parser.add_argument("--cp-verbose-init", action="store_true", help="command processor enable debug of init")
parser.add_argument("--cp-verbose-requests", action="store_true", help="command processor enable debug of requests")
parser.add_argument("--cp-verbose-responses", action="store_true", help="command processor enable debug of responses")

arguments = parser.parse_args()

###################
# router id bases #
###################
COMPUTETILE_RTR_ID = 0
SHAREDMEM_RTR_ID = 1024
CHIPRTR_ID = 1024*1024

SYSCONFIG = {
    "sys_num_pxn" : 1,
    "sys_pxn_pods" : 1,
    "sys_pod_cores" : 8,
    "sys_core_threads" : 16,
    "sys_core_clock" : "1GHz",
    "sys_pod_l2_banks" : L2SPRange.L2SP_POD_BANKS,
    "sys_pod_dram_ports" : MainMemoryRange.POD_MAINMEM_BANKS,
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
    "debug_memory": False,
    "debug_syscalls" : False,
}

SYSCONFIG['sys_num_pxn'] = arguments.num_pxn
SYSCONFIG['sys_pxn_pods'] = arguments.pxn_pods
SYSCONFIG['sys_pod_cores'] = arguments.pod_cores
SYSCONFIG['sys_core_threads'] = arguments.core_threads
SYSCONFIG['sys_cp_present'] = bool(arguments.with_command_processor)

CORE_DEBUG['debug_memory'] = arguments.debug_memory
CORE_DEBUG['debug_requests'] = arguments.debug_requests
CORE_DEBUG['debug_responses'] = arguments.debug_responses
CORE_DEBUG['debug_syscalls'] = arguments.debug_syscalls
CORE_DEBUG['debug_init'] = arguments.debug_init
CORE_DEBUG['debug_clock'] = arguments.debug_clock

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
            "destinations" : "1,2",
            "verbose_level" : arguments.verbose_memory,
        })

    def __init__(self, pod=0, pxn=0):
        self.id = self.CORE_ID
        self.pod = pod
        self.pxn = pxn
        self.initCore()
