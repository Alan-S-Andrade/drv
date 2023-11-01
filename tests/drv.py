# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

import argparse

################################
# parse command line arguments #
################################
parser = argparse.ArgumentParser()
parser.add_argument("program", help="program to run")
parser.add_argument("argv", nargs=argparse.REMAINDER, help="arguments to program")
parser.add_argument("--verbose", type=int, default=0, help="verbosity of core")
parser.add_argument("--dram-backend", type=str, default="simple", choices=['simple', 'ramulator'], help="backend timing model for DRAM")
parser.add_argument("--debug-memory", action="store_true", help="enable memory debug")
parser.add_argument("--debug-requests", action="store_true", help="enable debug of requests")
parser.add_argument("--debug-responses", action="store_true", help="enable debug of responses")
parser.add_argument("--debug-syscalls", action="store_true", help="enable debug of syscalls")
parser.add_argument("--verbose-memory", type=int, default=0, help="verbosity of memory")

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

CORE_DEBUG['debug_memory'] = arguments.debug_memory
CORE_DEBUG['debug_requests'] = arguments.debug_requests
CORE_DEBUG['debug_responses'] = arguments.debug_responses
CORE_DEBUG['debug_syscalls'] = arguments.debug_syscalls
