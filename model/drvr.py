from system import *
from cmdline import parse_args
from pandohammer import PANDOHammer
import argparse

pandohammer = PANDOHammer(parse_args(), core_builder = RCoreBuilder)

# create the system
print("Building system")
pandohammer.build()

print("Running simualation")
