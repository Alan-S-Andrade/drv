import sst
import sys

DEBUG_MEM = 1

#executable = sys.argv[1]
if (len(sys.argv) < 2):
    print("ERROR: Must specify executable to run")
    exit(1)

executable = sys.argv[1]

core = sst.Component("core", "Drv.DrvCore")
core.addParams({
    "verbose" : 100,
    "debug_init" : True,
    "debug_clock" : True,
    "executable" : executable,
})
iface = core.setSubComponent("memory", "memHierarchy.standardInterface")

memctrl = sst.Component("memctrl", "memHierarchy.MemController")
memctrl.addParams({
    "debug" : DEBUG_MEM,
    "debug_level" : 100,
    "verbose" : 100,
    "clock" : "1GHz",
    "addr_range_start" : 0,
    "addr_range_end" : 512*1024*1024-1,
    })
memory = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
memory.addParams({
    "access_time" : "32ns",
    "mem_size" : "512MiB",
})

link_core_mem = sst.Link("link_core_mem")
link_core_mem.connect((iface, "port", "8ns"), (memctrl, "direct_link", "8ns"))

