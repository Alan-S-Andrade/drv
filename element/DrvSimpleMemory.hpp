#pragma once
#include <DrvMemory.hpp>
#include <string>
#include <vector>

namespace SST {
namespace Drv {

/**
 * @brief A simple memory class
 *
 * This memory class is has a constant latency and built-in data store.
 */
class DrvSimpleMemory : public DrvMemory {
public:
    /**
     * @brief Construct a new DrvSimpleMemory object
     */
    DrvSimpleMemory() {}

    /**
     * @brief Destroy the DrvSimpleMemory object
     */
    virtual ~DrvSimpleMemory() {}

    /**
     * @brief Send a memory request
     */
    virtual void sendRequest(DrvCore *core, DrvThread *thread, const std::shared_ptr<DrvAPI::DrvAPIMem> & thread_mem_req) override;

private:
    /**
     * @brief Send a read request
     */
    void sendWriteRequest(DrvCore *core, DrvThread *thread, const std::shared_ptr<DrvAPI::DrvAPIMemWrite> &write_req);

    /**
     * @brief Send a write request
     */
    void sendReadRequest(DrvCore *core, DrvThread *thread, const std::shared_ptr<DrvAPI::DrvAPIMemRead> &read_req);
    
    // members
    std::vector<uint8_t> data_; //!< The data store
};

}
}
