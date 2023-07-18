#pragma once
#include <sst/core/component.h>
#include <sst/core/link.h>
#include <DrvAPIAddress.hpp>
#include <DrvAPIThreadState.hpp>
#include <memory>

namespace SST {
namespace Drv {

/* Forward declarations */
class DrvCore;
class DrvThread;

/**
 * @brief The memory class
 * 
 */
class DrvMemory {
public:
    /**
     * @brief Construct a new DrvMemory object
     */
    DrvMemory() {}

    /**
     * @brief Destroy the DrvMemory object
     */
    virtual ~DrvMemory() {}
    
    /**
     * @brief Send a memory request
     */
    virtual void sendRequest(DrvCore *core
                             ,DrvThread *thread
                             ,const std::shared_ptr<DrvAPI::DrvAPIMem> & thread_mem_req) = 0;
};

}
}
