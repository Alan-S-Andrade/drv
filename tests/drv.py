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
parser.add_argument("--debug-init", action="store_true", help="enable debug of init")
parser.add_argument("--debug-memory", action="store_true", help="enable memory debug")
parser.add_argument("--debug-requests", action="store_true", help="enable debug of requests")
parser.add_argument("--debug-responses", action="store_true", help="enable debug of responses")
parser.add_argument("--debug-syscalls", action="store_true", help="enable debug of syscalls")
parser.add_argument("--verbose-memory", type=int, default=0, help="verbosity of memory")
parser.add_argument("--pod-cores", type=int, default=8, help="number of cores per pod")
parser.add_argument("--pxn-pods", type=int, default=1, help="number of pods")
parser.add_argument("--core-threads", type=int, default=16, help="number of threads per core")

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

SYSCONFIG['sys_pxn_pods'] = arguments.pxn_pods
SYSCONFIG['sys_pod_cores'] = arguments.pod_cores
SYSCONFIG['sys_core_threads'] = arguments.core_threads

CORE_DEBUG['debug_memory'] = arguments.debug_memory
CORE_DEBUG['debug_requests'] = arguments.debug_requests
CORE_DEBUG['debug_responses'] = arguments.debug_responses
CORE_DEBUG['debug_syscalls'] = arguments.debug_syscalls
CORE_DEBUG['debug_init'] = arguments.debug_init
