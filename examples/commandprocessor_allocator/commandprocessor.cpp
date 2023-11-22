// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington
#include <DrvAPI.hpp>
#include <string>
#include <cstdio>
#include <inttypes.h>
using namespace DrvAPI;

DrvAPIGlobalL2SP<int64_t> lock;

int CPMain(int argc, char *argv[])
{
    DrvAPIMemoryAllocatorInit();

    DrvAPIPointer<void> p = DrvAPIMemoryAlloc
        (DrvAPIMemoryL2SP,1024);

    printf("p = %" PRIx64 "\n", static_cast<DrvAPIAddress>(p));

    return 0;
}

declare_drv_api_main(CPMain);
