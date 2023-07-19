#include "DrvStdMemory.hpp"
#include "DrvCore.hpp"
#include "DrvThread.hpp"

using namespace SST;
using namespace Drv;
using namespace Interfaces;

/**
 * @brief Construct a new DrvStdMemory object
 * 
 * @param core 
 * @param mem_name 
 */
DrvStdMemory::DrvStdMemory(DrvCore* core, const std::string &mem_name)
    : core_(core)
    , mem_name_(mem_name) {
    core_->output()->verbose(CALL_INFO, 2, DrvCore::DEBUG_INIT,
                             "Creating memory '%s'\n", mem_name_.c_str());

    mem_ = core_->loadStandardMemSubComponent
        (mem_name_, ComponentInfo::SHARE_NONE, new SST::Interfaces::StandardMem::Handler<DrvStdMemory>(this, &DrvStdMemory::handleEvent));

    if (!mem_) {
        core_->output()->fatal(CALL_INFO, -1, "Failed to load memory '%s'\n", mem_name_.c_str());
    }

    core_->output()->verbose(CALL_INFO, 2, DrvCore::DEBUG_INIT,
                             "Memory '%s' created: %p\n", mem_name_.c_str(), mem_);
}

/**
 * @brief Destroy the DrvStdMemory object
 * 
 */
DrvStdMemory::~DrvStdMemory() {
    core_->output()->verbose(CALL_INFO, 2, DrvCore::DEBUG_INIT,
                             "Destroying memory '%s'\n", mem_name_.c_str());
}

/**
 * @brief Send a memory request
 * 
 * @param core 
 * @param thread 
 * @param mem_req 
 */
void
DrvStdMemory::sendRequest(DrvCore *core
                          ,DrvThread *thread
                          ,const std::shared_ptr<DrvAPI::DrvAPIMem> &mem_req) {
    core->output()->verbose(CALL_INFO, 10, DrvCore::DEBUG_CLK,
                            "Sending memory request to '%s'\n", mem_name_.c_str());

    auto write_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemWrite>(mem_req);
    if (write_req) {
        /* do write */
        uint64_t size = write_req->getSize();
        uint64_t addr = write_req->getAddress().offset();
        std::vector<uint8_t> data(size);
        write_req->getPayload(data.data());
        Interfaces::StandardMem::Request *req = new Interfaces::StandardMem::Write(addr, size, data);
        mem_->send(req);
        return;
    }

    auto read_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemRead>(mem_req);
    if (read_req) {
        /* do read */
        return;
    }

    auto atomic_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemAtomic>(mem_req);
    if (atomic_req) {
        /* do atomic */
        return;
    }
}

/**
 * @brief handleEvent is called when a message is received
 * 
 * @param req
 */
void
DrvStdMemory::handleEvent(SST::Interfaces::StandardMem::Request *req) {
    core_->output()->verbose(CALL_INFO, 10, DrvCore::DEBUG_REQ,
                             "Received memory request from '%s'\n", mem_name_.c_str());
}

