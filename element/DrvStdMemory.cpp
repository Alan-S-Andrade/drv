// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include "DrvStdMemory.hpp"
#include "DrvCustomStdMem.hpp"
#include "DrvCore.hpp"
#include "DrvThread.hpp"
#include "DrvAPIInfo.hpp"
#include "DrvAPIAddressMap.hpp"
#include <sst/elements/memHierarchy/memoryController.h>

using namespace SST;
using namespace Drv;
using namespace Interfaces;


/**
 * @brief Construct a new DrvStdMemory object
 * 
 * @param id
 * @param params
 */
DrvStdMemory::DrvStdMemory(SST::ComponentId_t id, SST::Params& params, DrvCore *core)
    : DrvMemory(id, params, core) {
    mem_ = loadUserSubComponent<Interfaces::StandardMem>
        ("memory", ComponentInfo::SHARE_NONE,
         core->getClockTC(),
         new SST::Interfaces::StandardMem::Handler<DrvStdMemory>(this, &DrvStdMemory::handleEvent));
    
    if (mem_ == nullptr) {
        SST::Params mem_params = params.get_scoped_params("memory.");
        mem_ = loadAnonymousSubComponent<Interfaces::StandardMem>
            ("memHierarchy.standardInterface", "memory", 0,
             ComponentInfo::SHARE_NONE,
             mem_params,
             core->getClockTC(),
             new SST::Interfaces::StandardMem::Handler<DrvStdMemory>(this, &DrvStdMemory::handleEvent));        
    }
    DrvAPI::DrvAPIPAddress mmio_start;
    mmio_start.type() = DrvAPI::DrvAPIPAddress::TYPE_CTRL;
    mmio_start.pxn() = core->pxn_;
    mmio_start.pod() = core->pod_;
    mmio_start.core_y() = DrvAPI::coreYFromId(core->id_);
    mmio_start.core_x() = DrvAPI::coreXFromId(core->id_);
    mmio_start.ctrl_offset() = 0;
    mem_->setMemoryMappedAddressRegion(mmio_start.encode(), 1<<DrvAPI::DrvAPIPAddress::CtrlOffsetHandle::bits());
}

/**
 * @brief Destroy the DrvStdMemory object
 * 
 */
DrvStdMemory::~DrvStdMemory() {
    delete mem_;
}

/**
 * @brief translate a pgas pointer to a native pointer
 */
void
DrvStdMemory::toNativePointer(DrvAPI::DrvAPIAddress paddr, void **ptr, size_t *size) {
    /* find the range this memory address belongs to */
    auto it = std::lower_bound(SST::MemHierarchy::MemController::AddrRangeToMC.begin(),
                               SST::MemHierarchy::MemController::AddrRangeToMC.end(),
                               std::make_tuple(paddr, paddr, nullptr),
                               [](const std::tuple<uint64_t, uint64_t, SST::MemHierarchy::MemController*> &element,
                                  const std::tuple<uint64_t, uint64_t, SST::MemHierarchy::MemController*> &value) {
                                   return std::get<0>(element) <= std::get<0>(value);
                               });
    uint64_t addr_range_start, addr_range_stop;
    SST::MemHierarchy::MemController *memory_controller;
    if (it == SST::MemHierarchy::MemController::AddrRangeToMC.end() ||
        it == SST::MemHierarchy::MemController::AddrRangeToMC.begin()) {
        output_.fatal(CALL_INFO, -1,
                      "Could not find memory controller for address %" PRIx64 "\n",
                      paddr);
    }

    std::tie(addr_range_start, addr_range_stop, memory_controller) = *(--it);

    // check that the address is within the range
    if (paddr < addr_range_start || paddr > addr_range_stop) {
        output_.fatal(CALL_INFO, -1,
                      "Could not find memory controller for address %" PRIx64 "\n",
                      paddr);
    }

    auto *backing = dynamic_cast<SST::MemHierarchy::Backend::BackingMMAP*>
        (memory_controller->backing_);
    /* we only support if the backing store is a BackingMMAP */
    if (!backing) {
        output_.fatal(CALL_INFO, -1,
                      "Memory controller does not have a BackingMMAP "
                      "required for translation to native pointer\n");
    }
    uint64_t lpaddr = memory_controller->translateToLocal(paddr);
    uint8_t *bptr = &backing->m_buffer[lpaddr];
    *ptr = bptr;
    *size = backing->m_size - lpaddr;
    return;

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
        uint64_t addr = write_req->getAddress();
        output_.verbose(CALL_INFO, 10, DrvMemory::VERBOSE_REQ,
                        "Sending write request addr=%" PRIx64 " size=%" PRIu64 "\n",
                        addr, size);
        std::vector<uint8_t> data(size);
        write_req->getPayload(&data[0]);
        StandardMem::Write *req = new StandardMem::Write(addr, size, data);
        req->tid = core->getThreadID(thread);
        // add statistic
        core->addStoreStat(DrvAPI::DrvAPIPAddress{addr});
        mem_->send(req);
        return;
    }

    auto read_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemRead>(mem_req);
    if (read_req) {
        /* do read */
        uint64_t size = read_req->getSize();
        uint64_t addr = read_req->getAddress();
        output_.verbose(CALL_INFO, 10, DrvMemory::VERBOSE_REQ,
                                "Sending read request addr=%" PRIx64 " size=%" PRIu64 "\n",
                                addr, size);
        StandardMem::Read *req = new StandardMem::Read(addr, size);
        req->tid = core->getThreadID(thread);
        core->addLoadStat(DrvAPI::DrvAPIPAddress{addr});
        mem_->send(req);
        return;
    }

    auto to_native_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIToNativePointer>(mem_req);
    if (to_native_req) {
        uint64_t paddr = to_native_req->getAddress();
        void *ptr=nullptr;
        size_t size=0;
        toNativePointer(paddr, &ptr, &size);
        to_native_req->setNativePointer(ptr);
        to_native_req->setRegionSize(size);
        to_native_req->complete();
        return;
    }

    auto atomic_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemAtomic>(mem_req);
    if (atomic_req) {
        /* do atomic */
        /* we implement the atomic in terms of read-lock and write-unlockprovided by stdMem.h
         * we could also do this with ll and sc, but those do not guarantee success
         */
        uint64_t size = atomic_req->getSize();
        uint64_t addr = atomic_req->getAddress();
        output_.verbose(CALL_INFO, 10, DrvMemory::VERBOSE_REQ,
                        "Sending atomic request addr=%" PRIx64 " size=%" PRIu64 "\n",
                        addr, size);
        core->addAtomicStat(DrvAPI::DrvAPIPAddress{addr});
        AtomicReqData *data = new AtomicReqData();
        data->pAddr = addr;
        data->size = size;
        data->wdata.resize(size);
        data->opcode = atomic_req->getOp();
        atomic_req->getPayload(&data->wdata[0]);
        if (atomic_req->hasExt()) {
            data->extdata.resize(size);
            atomic_req->getPayloadExt(&data->extdata[0]);
        }
        // set atomic type
        StandardMem::CustomReq *req = new StandardMem::CustomReq(data);
        req->tid = core->getThreadID(thread);
        mem_->send(req);
        return;
    }

    // fatally error if we don't know the request type
    if (!(write_req || read_req || to_native_req || atomic_req)) {
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
    output_.verbose(CALL_INFO, 10, DrvMemory::VERBOSE_REQ, "Received memory request\n");
    DrvThread *thread = nullptr;
    std::shared_ptr<DrvAPI::DrvAPIMem> mem_req = nullptr;
    auto write_rsp = dynamic_cast<StandardMem::WriteResp*>(req);
    if (write_rsp) {
        thread = core_->getThread(write_rsp->tid);
        if (!thread) {
            output_.fatal(CALL_INFO, -1, "Failed to find thread for tid=%" PRIu32 "\n", write_rsp->tid);
        }
        
        output_.verbose(CALL_INFO, 10, DrvMemory::VERBOSE_REQ,
                        "Received write response from addr=%" PRIx64 " size=%" PRIu64 "\n",
                        write_rsp->pAddr, write_rsp->size);
        // complete write
        mem_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMem>(thread->getAPIThread().getState());
        if (mem_req) {
            mem_req->complete();
        } else {
            output_.fatal(CALL_INFO, -1, "Failed to find memory request for tid=%" PRIu32 "\n", write_rsp->tid);
        }
    }

    auto read_rsp = dynamic_cast<StandardMem::ReadResp*>(req);
    if (read_rsp) {
        thread = core_->getThread(read_rsp->tid);
        if (!thread) {
            output_.fatal(CALL_INFO, -1, "Failed to find thread for tid=%" PRIu32 "\n", read_rsp->tid);
        }
        output_.verbose(CALL_INFO, 10, DrvMemory::VERBOSE_REQ,
                        "Received read response from addr=%" PRIx64 " size=%" PRIu64 "\n",
                        read_rsp->pAddr, read_rsp->size);

        // complete read, if this was a read request
        auto read_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemRead>(thread->getAPIThread().getState());
        if (read_req) {
            read_req->setResult(&read_rsp->data[0]);
            read_req->complete();            
        }

        if (!read_req) {
            output_.fatal(CALL_INFO, -1, "Failed to find memory request for tid=%" PRIu32 "\n", read_rsp->tid);
        }
    }

    auto custom_rsp = dynamic_cast<StandardMem::CustomResp*>(req);
    AtomicReqData* areq_data = nullptr;
    if (custom_rsp) {
        areq_data = dynamic_cast<AtomicReqData*>(custom_rsp->data);
        if (areq_data) {
            output_.verbose(CALL_INFO, 10, DrvMemory::VERBOSE_REQ,
                            "Received custom response\n");            
            DrvThread *thread = core_->getThread(custom_rsp->tid);
            auto atomic_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemAtomic>(thread->getAPIThread().getState());
            if (atomic_req) {
                atomic_req->setResult(&areq_data->rdata[0]);
                atomic_req->complete();
            } else {
                output_.fatal(CALL_INFO, -1, "Failed to find memory request for tid=%" PRIu32 "\n", custom_rsp->tid);
            }
        }
        delete areq_data;
    }

    auto write_req = dynamic_cast<StandardMem::Write*>(req);
    if (write_req) {
        output_.verbose(CALL_INFO, 10, DrvMemory::VERBOSE_REQ,
                        "Received write request addr=%" PRIx64 " size=%" PRIu64 "\n",
                        write_req->pAddr, write_req->size);
        core_->handleMMIOWriteRequest(write_req);
        mem_->send(write_req->makeResponse());
    }


    // must delete the request
    delete req;

    // fatally error if we don't know the response type
    if (!(write_rsp || read_rsp || (custom_rsp && areq_data) || write_req)) {
        output_.fatal(CALL_INFO, -1, "Unknown memory response type\n");
    }
    core_->assertCoreOn();
}

