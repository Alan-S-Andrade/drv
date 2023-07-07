import sst

core = sst.Component("core", "Fwd.FwdCore")
core.addParams({
    "verbose" : 100,
    "debug_init" : True,
    "debug_clock" : True,
})
