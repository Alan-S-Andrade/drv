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
    auto write_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemWrite>(mem_req);
    if (write_req) {
        /* do write */
        uint64_t size = write_req->getSize();
        uint64_t addr = write_req->getAddress().offset();
        core->output()->verbose(CALL_INFO, 10, DrvCore::DEBUG_CLK,
                                "Sending write request to '%s' addr=%" PRIx64 " size=%" PRIu64 "\n",
                                mem_name_.c_str(), addr, size);
        std::vector<uint8_t> data(size);
        write_req->getPayload(&data[0]);
        StandardMem::Write *req = new StandardMem::Write(addr, size, data);
        req->tid = core->getThreadID(thread);
        mem_->send(req);
        return;
    }

    auto read_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemRead>(mem_req);
    if (read_req) {
        /* do read */
        uint64_t size = read_req->getSize();
        uint64_t addr = read_req->getAddress().offset();
        core->output()->verbose(CALL_INFO, 10, DrvCore::DEBUG_CLK,
                                "Sending read request to '%s' addr=%" PRIx64 " size=%" PRIu64 "\n",
                                mem_name_.c_str(), addr, size);
        StandardMem::Read *req = new StandardMem::Read(addr, size);
        req->tid = core->getThreadID(thread);
        mem_->send(req);
        return;
    }

    auto atomic_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemAtomic>(mem_req);
    if (atomic_req) {
        /* do atomic */
        /* we implement the atomic in terms of read-lock and write-unlockprovided by stdMem.h
         * we could also do this with ll and sc, but those do not guarantee success
         */
        uint64_t size = atomic_req->getSize();
        uint64_t addr = atomic_req->getAddress().offset();
        core->output()->verbose(CALL_INFO, 10, DrvCore::DEBUG_CLK,
                                "Sending atomic (read-lock) request to '%s' addr=%" PRIx64 " size=%" PRIu64 "\n",
                                mem_name_.c_str(), addr, size);
        StandardMem::ReadLock *req = new StandardMem::ReadLock(addr, size);
        req->tid = core->getThreadID(thread);
        mem_->send(req);
        return;
    }

    // fatally error if we don't know the request type
    if (!(write_req || read_req || atomic_req)) {
        core->output()->fatal(CALL_INFO, -1, "Unknown memory request type\n");
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
    DrvThread *thread = nullptr;
    std::shared_ptr<DrvAPI::DrvAPIMem> mem_req = nullptr;
    auto write_rsp = dynamic_cast<StandardMem::WriteResp*>(req);
    if (write_rsp) {
        thread = core_->getThread(write_rsp->tid);
        if (!thread) {
            core_->output()->fatal(CALL_INFO, -1, "Failed to find thread for tid=%" PRIu32 "\n", write_rsp->tid);
        }
        core_->output()->verbose(CALL_INFO, 10, DrvCore::DEBUG_REQ,
                                 "Received write response from '%s' addr=%" PRIx64 " size=%" PRIu64 "\n",
                                 mem_name_.c_str(), write_rsp->pAddr, write_rsp->size);
        // complete write
        mem_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMem>(thread->getAPIThread().getState());
        if (mem_req) {
            mem_req->complete();
        } else {
            core_->output()->fatal(CALL_INFO, -1, "Failed to find memory request for tid=%" PRIu32 "\n", write_rsp->tid);
        }
    }

    auto read_rsp = dynamic_cast<StandardMem::ReadResp*>(req);
    if (read_rsp) {
        thread = core_->getThread(read_rsp->tid);
        if (!thread) {
            core_->output()->fatal(CALL_INFO, -1, "Failed to find thread for tid=%" PRIu32 "\n", read_rsp->tid);
        }
        core_->output()->verbose(CALL_INFO, 10, DrvCore::DEBUG_REQ,
                                 "Received read response from '%s' addr=%" PRIx64 " size=%" PRIu64 "\n",
                                 mem_name_.c_str(), read_rsp->pAddr, read_rsp->size);        

        // complete read, if this was a read request
        auto read_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemRead>(thread->getAPIThread().getState());
        if (read_req) {
            read_req->setResult(&read_rsp->data[0]);
            read_req->complete();            
        }

        // perform modify-write, if this was an atomic request
        auto atomic_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemAtomic>(thread->getAPIThread().getState());
        if (atomic_req) {
            // read the result and modify it
            atomic_req->setResult(&read_rsp->data[0]);
            atomic_req->modify();
            // now send a write-unlock
            std::vector<uint8_t> data(atomic_req->getSize());
            atomic_req->getPayload(&data[0]);
            StandardMem::WriteUnlock *wureq = new StandardMem::WriteUnlock(read_rsp->pAddr, read_rsp->size, data);
            wureq->tid = read_rsp->tid;
            core_->output()->verbose(CALL_INFO, 10, DrvCore::DEBUG_REQ,
                                     "Sending write-unlock request to '%s' addr=%" PRIx64 " size=%" PRIu64 "\n",
                                     mem_name_.c_str(), read_rsp->pAddr, read_rsp->size);
            mem_->send(wureq);
        }

        if (!(read_req || atomic_req)) {
            core_->output()->fatal(CALL_INFO, -1, "Failed to find memory request for tid=%" PRIu32 "\n", read_rsp->tid);
        }
    }

    // must delete the request
    delete req;

    // fatally error if we don't know the response type
    if (!(write_rsp || read_rsp)) {
        core_->output()->fatal(CALL_INFO, -1, "Unknown memory response type\n");
    }
}

