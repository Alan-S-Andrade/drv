import sst

core = sst.Component("core", "Drv.DrvCore")
core.addParams({
    "verbose" : 100,
    "debug_init" : True,
    "debug_clock" : True,
})
