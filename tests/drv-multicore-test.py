import sst
import sys

VERBOSE = 0
CORES = 2

#executable = sys.argv[1]
if (len(sys.argv) < 2):
    print("ERROR: Must specify executable to run")
    exit(1)

executable = sys.argv[1]

# build a a shared memory
memctrl = sst.Component("memctrl", "memHierarchy.MemController")
memctrl.addParams({
    "clock" : "1GHz",
    "addr_range_start" : 0,
    "addr_range_end" : 512*1024*1024-1,
})
memory = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
memory.addParams({
    "access_time" : "1ns",
    "mem_size" : "512MiB",
})

# build a bus
bus = sst.Component("bus", "memHierarchy.Bus")
bus.addParams({
    "bus_frequency" : "1GHz",
    "idle_max" : 0,
})

# connect the bus to the memory
bus_memctrl_link = sst.Link("bus_memctrl_link")
bus_memctrl_link.connect(
    (bus, "low_network_0", "1ns"),
    (memctrl, "direct_link", "1ns")
)

# build the cores
cores = []
for i in range(CORES):
    core = sst.Component("core_%d" % i, "Drv.DrvCore")
    core.addParams({
        "verbose" : VERBOSE,
        "debug_init" : True,
        "debug_clock" : True,
        "debug_requests" : True,
        "debug_response" : True,
        "executable" : executable,
        "id" : i,
    })
    cores.append(core)    

# connect the cores to the bus    
for (idx, core) in enumerate(cores):
    iface = core.setSubComponent("memory", "memHierarchy.standardInterface")
    iface.addParams({
        "verbose" : VERBOSE,
    })
    core_bus_link = sst.Link("core_bus_link_%d" % idx)
    core_bus_link.connect(
        (iface, "port", "1ns"),
        (bus, "high_network_%d" % idx, "1ns")
    )
