// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#pragma once
#include <DrvAPIAddress.hpp>
#include "DrvMemory.hpp"
#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/event.h>

namespace SST {
namespace Drv {
/**
 * @brief Memory model that uses sst standard memory
 * 
 */
class DrvStdMemory : public DrvMemory {
public:
    // register this subcomponent into the element library
    SST_ELI_REGISTER_SUBCOMPONENT(
        SST::Drv::DrvStdMemory,
        "Drv",
        "DrvStdMemory",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Memory that interfaces with memHierarchy components",
        SST::Drv::DrvMemory
    )

    // register subcomponent slots
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"memory", "Memory component", "SST::Interfaces::StandardMem"}
    )
    
    /**
     * @brief Construct a new DrvStdMemory object
     * 
     * @param id
     * @param params
     * @param core
     */
    DrvStdMemory(SST::ComponentId_t id, SST::Params& params, DrvCore *core);

    /**
     * @brief Destroy the DrvStdMemory object
     * 
     */
    virtual ~DrvStdMemory();

    /**
     * @brief Send a memory request
     * 
     * @param core 
     * @param thread 
     * @param mem_req 
     */
    void sendRequest(DrvCore *core
                     ,DrvThread *thread
                     ,const std::shared_ptr<DrvAPI::DrvAPIMem> &mem_req);

    /**
     * @brief init is called at the beginning of the simulation
     */
    void init(unsigned int phase) { mem_->init(phase); }

    /**
     * @brief setup is called during setup phase
     */
    void setup() { mem_->setup(); }

    /**
     * @brief finish is called at the end of the simulation
     */
    void finish() { mem_->finish(); }

    /**
     * @brief translate a pgas pointer to a native pointer
     */
    void toNativePointer(DrvAPI::DrvAPIAddress addr, void **ptr, size_t *size);

private:
    /**
     * @brief handleEvent is called when a message is received
     * 
     * @param req
     */
    void handleEvent(SST::Interfaces::StandardMem::Request *req);
    
    Interfaces::StandardMem *mem_; //!< The memory
};

}
}
