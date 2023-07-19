#pragma once
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
    /**
     * @brief Construct a new DrvStdMemory object
     * 
     * @param core 
     * @param mem_name 
     */
    DrvStdMemory(DrvCore* core, const std::string &mem_name);

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

private:
    /**
     * @brief handleEvent is called when a message is received
     * 
     * @param req
     */
    void handleEvent(SST::Interfaces::StandardMem::Request *req);
    
    DrvCore *core_; //!< The core
    std::string mem_name_; //!< The name of the memory
    Interfaces::StandardMem *mem_; //!< The memory
};

}
}
