import sst
import sys

DEBUG_MEM = 0
VERBOSE = 0
CORES = 2

print("Running Drv Memhierarcy Test 2: Using an MultithreadL1, LLC, and a MemController")

if (len(sys.argv) < 2):
    print("ERROR: Must specify executable to run")
    exit(1)

executable = sys.argv[1]

cores = []
# build cores
for i in range(CORES):
    core = sst.Component("core%d" % i, "Drv.DrvCore")
    core.addParams({
        "verbose" : VERBOSE,
        "debug_init" : True,
        "debug_clock" : True,
        "debug_requests" : True,
        "executable" : executable,
        "id" : i,
    })
    cores.append(core)

# build a shared l1 interface
mtl1 = sst.Component("mtl1", "memHierarchy.multithreadL1")
mtl1.addParams({
    "clock" : "1GHz",
    "requests_per_cycle" : 0,
    "responses_per_cycle" : 0,
})

# build  an llc
llc = sst.Component("llc", "memHierarchy.Cache")
llc.addParams({
    "debug" : DEBUG_MEM,
    "debug_level" : VERBOSE,
    "verbose" : VERBOSE,
    "cache_frequency" : "1GHz",
    "associativity" : 1,
    "L1" : 1,
    "cache_line_size" : 64,
    "cache_size" : "2KiB",
    "access_latency_cycles" : 1,
    "coherence_protocol" : "NONE",
    "cache_type"  : "inclusive",
})

# build the memory controller
memctrl = sst.Component("memctrl", "memHierarchy.MemController")
memctrl.addParams({
    "debug" : DEBUG_MEM,
    "debug_level" : VERBOSE,
    "verbose" : VERBOSE,
    "clock" : "1GHz",
    "addr_range_start" : 0,
    "addr_range_end" : 512*1024*1024-1,
    })
memory = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
memory.addParams({
    "access_time" : "1ns",
    "mem_size" : "512MiB",
})

# connect the llc to the memory controller
link_llc_mem = sst.Link("link_llc_mem")
link_llc_mem.connect(
    (llc, "low_network_0", "1ns"),
    (memctrl, "direct_link", "1ns"))

# connect the core to the shared l1 interface

for (idx, core) in enumerate(cores):
    iface = core.setSubComponent("memory", "memHierarchy.standardInterface")
    iface.addParams({
        "verbose" : VERBOSE,
    })
    link_core_mtl1 = sst.Link("link_core_mtl1%d" % idx)
    link_core_mtl1.connect(
        (iface, "port", "1ns"),
        (mtl1, "thread%d" % idx, "1ns"))

# connect the shared l1 interface to the llc
link_mtl1_llc = sst.Link("link_mtl1_llc")
link_mtl1_llc.connect(
    (mtl1, "cache", "1ns"),
    (llc, "high_network_0", "1ns"))

