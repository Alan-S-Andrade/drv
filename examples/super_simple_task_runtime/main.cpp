// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington
#include <stdio.h>
#include <DrvAPI.hpp>
#include <inttypes.h>
#include "common.h"
#include "task.h"

using namespace DrvAPI;

int pandoMain(int argc, char *argv[])
{
    pr_info("hello, from main\n");
    uint32_t pod=0, core=0;
    pod = numPXNPods()-1;
    core = numPodCores()-1;

    pr_info("running a task on pod %" PRIu32 ", core %" PRIu32 "\n"
           ,myPodId()
           ,myCoreId()
           );

    DrvAPIPointer<int64_t> done = DrvAPIMemoryAlloc(DrvAPIMemoryL2SP, sizeof(int64_t));
    pr_info("done = %" PRIx64 "\n", static_cast<DrvAPIAddress>(done));
    *done =  0;
    execute_on(myPXNId(), pod, core, newTask([done](){
        pr_info("hello, from task\n");
        *done = 1;
    }));

    while (*done != 1)
        nop(1000);

    DrvAPIMemoryFree(done);
    return 0;
}
