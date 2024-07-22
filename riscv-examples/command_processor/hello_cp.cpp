// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include <DrvAPI.hpp>
#include <pandocommand/executable.hpp>
#include <pandocommand/control.hpp>
#include <pandocommand/loader.hpp>
using namespace pandocommand;
using namespace DrvAPI;
int CommandProcessor(int argc, char *argv[])
{
    printf("hello, from the command processor!\n");
    for (int i = 0; i < argc; i++) {
        printf("argv[%d] = %s\n", i, argv[i]);
    }

    auto exe = PANDOHammerExe::Open(argv[1]);
    loadProgram(*exe);

    DrvAPIPointer<uint64_t> signal_p = exe->symbol<uint64_t>("signal", {0,0,0});
    assertResetAll(false);    
    *signal_p = 1;
    return 0;
}

declare_drv_api_main(CommandProcessor);
