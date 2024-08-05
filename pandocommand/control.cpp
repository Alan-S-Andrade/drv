// SPDX-License-Identifier: MIT
// Copyright (c) 2024 University of Washington
#include <pandocommand/control.hpp>
#include <DrvAPI.hpp>
using namespace DrvAPI;
using namespace pandocommand;

namespace pandocommand {

void assertResetAll(bool reset) {
    // release all cores from reset
    for (int pod = 0; pod < numPXNPods(); pod++) {
        for (int core = 0; core < numPodCores(); core++) {
            DrvAPIAddress reset_addr
                = absoluteCoreCtrlBase(myPXNId(), pod, core)
                + CTRL_CORE_RESET;
            write(reset_addr, reset ? 1l : 0l);
        }
    }
}

}
