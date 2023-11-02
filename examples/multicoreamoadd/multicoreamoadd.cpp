// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include <DrvAPI.hpp>
using namespace DrvAPI;

DrvAPIGlobalL2SP<int> counter;

int AmoaddMain(int argc, char *argv[])
{
    DrvAPIAddress addr = &counter;
    
    if (DrvAPIThread::current()->id() != 0)
        return 0;

    printf("core %2d: adding 1\n", DrvAPIThread::current()->coreId());
    int r = 0;    
    r = DrvAPI::atomic_add<uint64_t>(addr, 1);
    printf("core %2d: read %2d after amoadd\n", DrvAPIThread::current()->coreId(), r);
    
    while ((r = DrvAPI::read<uint64_t>(addr)) < 2)
        printf("core %2d: waiting for all cores: (%2d)\n", DrvAPIThread::current()->coreId(), r);
    
    return 0;
}

declare_drv_api_main(AmoaddMain);
