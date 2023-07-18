#include "DrvSimpleMemory.hpp"
using namespace SST;
using namespace Drv;

/**
 * @brief Send a read request
 */
void
DrvSimpleMemory::sendReadRequest(DrvCore *core, DrvThread *thread, const std::shared_ptr<DrvAPI::DrvAPIMemRead> &read_req) {
    read_req->setResult(&data_[read_req->getAddress().offset()]);
    read_req->complete();
}

/**
 * @brief Send a write request
 */
void
DrvSimpleMemory::sendWriteRequest(DrvCore *core, DrvThread *thread, const std::shared_ptr<DrvAPI::DrvAPIMemWrite> &write_req) {
    write_req->getPayload(&data_[write_req->getAddress().offset()]);
    write_req->complete();
}

/**
 * @brief Send a memory request
 */
void
DrvSimpleMemory::sendRequest (DrvCore *core, DrvThread *thread, const std::shared_ptr<DrvAPI::DrvAPIMem> & thread_mem_req) {
    auto read_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemRead>(thread_mem_req);
    if (read_req) {
        return sendReadRequest(core, thread, read_req);
    }

    auto write_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemWrite>(thread_mem_req);
    if (write_req) {
        return sendWriteRequest(core, thread, write_req);
    }

    return;
}




