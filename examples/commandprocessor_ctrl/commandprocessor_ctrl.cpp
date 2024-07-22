// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington
#include <DrvAPI.hpp>
#include <string>
#include <cstdio>
#include <inttypes.h>
using namespace DrvAPI;

DrvAPIGlobalDRAM<int64_t> done;

int CPMain(int argc, char *argv[])
{
    if (!isCommandProcessor()) {
        while (!done) {
            DrvAPI::wait(1000);
        }
    } else {
        // ctrl registers for pxn 0, pod 0, core 0
        DrvAPIAddress ctrl = absoluteCoreCtrlBase(0, 0, 0);
        DrvAPIAddressInfo ctrl_info = decodeAddress(ctrl);
        printf("ctrl      = 0x%" PRIx64 "\n", ctrl);
        printf("ctrl_info = %s\n", ctrl_info.to_string().c_str());
        DrvAPI::write(ctrl, 0xdeadbeef);
    }
    done = 1;
    return 0;
}

declare_drv_api_main(CPMain);
