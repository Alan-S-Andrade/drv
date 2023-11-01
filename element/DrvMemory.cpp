// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include <DrvMemory.hpp>

using namespace SST;
using namespace Drv;

DrvMemory::DrvMemory(SST::ComponentId_t id, SST::Params& params, DrvCore *core) 
    : SubComponent(id)
    , core_(core) {
    // get parameters
    bool verbose = params.find<bool>("verbose", false);

    // get debug masks
    uint32_t mask = 0;
    if (params.find<bool>("verbose_init", false))
        mask |= VERBOSE_INIT;
    if (params.find<bool>("verbose_requests", false))
        mask |= VERBOSE_REQ;
    if (params.find<bool>("verbose_responses", false))
        mask |= VERBOSE_RSP;
    
    // set up output
    output_.init("[DrvMemory @t:@f:@l: @p]", verbose, 0, SST::Output::STDOUT);
    output_.verbose(CALL_INFO, 1, DrvMemory::VERBOSE_INIT, "constructor done\n");
}

