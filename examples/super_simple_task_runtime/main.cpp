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
    // in this test just demonastrate that
    // pxn 0 can send pxn 1 a task
    if (myPXNId() != 0) {
        return 0;
    }

    pr_info("hello, from main\n");
    uint32_t pxn=0, pod=0, core=0;
    pxn = (numPXNs()-1) - myPXNId();
    pod = numPXNPods()-1 - myPodId();
    core = numPodCores()-1 - myCoreId();

    pr_info("running a task on pxn %" PRIu32 ", pod %" PRIu32 ", core %" PRIu32 "\n"
            ,pxn
            ,pod
            ,core
            );

    DrvAPIPointer<int64_t> done = DrvAPIMemoryAlloc(DrvAPIMemoryL2SP, sizeof(int64_t));
    pr_info("done = %" PRIx64 "\n", static_cast<DrvAPIAddress>(done));
    *done =  0;
    execute_on(pxn, pod, core, newTask([done](){
        pr_info("hello, from task\n");
        *done = 1;
    }));

    while (*done != 1)
        nop(1000);

    DrvAPIMemoryFree(done);
    return 0;
}
