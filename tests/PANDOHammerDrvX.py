# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

# Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
# Included multi node simulation

########################################################################################################
# Diagram of this model:                                                                               #
# https://docs.google.com/presentation/d/1ekm0MbExI1PKca5tDkSGEyBi-_0000Ro9OEaBLF-rUQ/edit?usp=sharing #
########################################################################################################
from drv import *
from drv_memory import *
from drv_tile import *
from drv_pandohammer import *

# for drvx we set the core clock to 125MHz (assumption is 1/8 ops are memory)
SYSCONFIG["sys_core_clock"] = "125MHz"

print("""
PANDOHammerDrvX:
  program = {}
""".format(
    arguments.program
))

MakeTile = lambda id, pod, pxn : DrvXTile(id, pod, pxn)
MakePANDOHammer(MakeTile)
