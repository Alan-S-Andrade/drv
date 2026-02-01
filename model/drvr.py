from system import *
from cmdline import parse_args
from pandohammer import PANDOHammer
import argparse
import sst

# Enable statistics output to CSV
sst.setStatisticOutput("sst.statOutputCSV", {
    "filepath": "stats.csv",
    "separator": ","
})
sst.setStatisticLoadLevel(1)

pandohammer = PANDOHammer(parse_args(), core_builder = RCoreBuilder)

# create the system
print("Building system")
pandohammer.build()

print("Running simualation")
