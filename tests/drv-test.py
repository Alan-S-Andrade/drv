import sst
import sys

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
