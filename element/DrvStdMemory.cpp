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


DrvStdMemory::ToNativeMetaData DrvStdMemory::to_native_meta_data_; //!< metadata for toNative function

void DrvStdMemory::ToNativeMetaData::init(DrvStdMemory *mem) {
    bool init = initialized.exchange(true);
    if (init) {
        return;
    }
    DrvCore *core = mem->core_;
    DrvAPI::DrvAPISysConfig cfg = core->sysConfig().config();
    l1sp_mcs.resize(cfg.numPXN(), std::vector<std::vector<record_type>>(cfg.numPXNPods()));
    l2sp_mcs.resize(cfg.numPXN(), std::vector<std::vector<record_type>>(cfg.numPXNPods()));
    dram_mcs.resize(cfg.numPXN());
    for (record_type &record : SST::MemHierarchy::MemController::AddrRangeToMC) {
        DrvAPI::DrvAPIAddress start, end;
        SST::MemHierarchy::MemController *mc;
        std::tie(start, end, mc) = record;
        DrvAPI::DrvAPIAddressInfo
            start_info = core->decoder().decode(start),
            end_info   = core->decoder().decode(end);

        int pxn = start_info.pxn();
        int pod = start_info.pod();
        if (start_info.is_l1sp()) {
            l1sp_mcs[pxn][pod].push_back(record);
        } else if (start_info.is_l2sp()) {
            l2sp_mcs[pxn][pod].push_back(record);
        } else if (start_info.is_dram()) {
            dram_mcs[pxn].push_back(record);
        }
    }
    // sort the records by start address
    for (int pxn = 0; pxn < cfg.numPXN(); pxn++) {
        for (int pod = 0; pod < cfg.numPXNPods(); pod++) {
            // check that we found one per core in pod
            if (l1sp_mcs[pxn][pod].size() != (size_t)cfg.numPodCores()) {
                mem->output_.fatal(CALL_INFO, -1, "Did not find correct number of L1SP banks for pod %d\n", pod);
            }
            std::sort(l1sp_mcs[pxn][pod].begin(), l1sp_mcs[pxn][pod].end(), [](const record_type &a, const record_type &b) {
                return std::get<0>(a) < std::get<0>(b);
            });
            // check that we found correct number of banks for pod
            if (l2sp_mcs[pxn][pod].size() != (size_t)cfg.podL2SPBankCount()) {
                mem->output_.fatal(CALL_INFO, -1, "Did not find correct number of L2SP banks for pod %d\n", pod);
            }
            std::sort(l2sp_mcs[pxn][pod].begin(), l2sp_mcs[pxn][pod].end(), [](const record_type &a, const record_type &b) {
                return std::get<0>(a) < std::get<0>(b);
            });
        }
        // check that we found correct number of dram banks for pxn
        if (dram_mcs[pxn].size() != (size_t)cfg.pxnDRAMPortCount()) {
            mem->output_.fatal(CALL_INFO, -1, "Did not find correct number of DRAM banks for pxn %d\n", pxn);
        }
        std::sort(dram_mcs[pxn].begin(), dram_mcs[pxn].end(), [](const record_type &a, const record_type &b) {
            return std::get<0>(a) < std::get<0>(b);
        });
    }

    l2sp_interleave_decode = {cfg.podL2SPInterleaveSize(), cfg.podL2SPBankCount()};
    dram_interleave_decode = {cfg.pxnDRAMInterleaveSize(), cfg.pxnDRAMPortCount()};
}

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
    DrvAPI::DrvAPIAddress mmio_start = params.find<DrvAPI::DrvAPIAddress>("memory_region_start", 0);
    DrvAPI::DrvAPIAddress mmio_size  = params.find<DrvAPI::DrvAPIAddress>("memory_region_size", 0x1000);
    output_.verbose(CALL_INFO, 0, 10, "Setting memory-mapped region to start at 0x%" PRIx64 " and size 0x%" PRIx64 "\n", mmio_start, mmio_size);
    mem_->setMemoryMappedAddressRegion(mmio_start, 0x1000);
}

/**
 * @brief Destroy the DrvStdMemory object
 * 
 */
DrvStdMemory::~DrvStdMemory() {
    delete mem_;
}

/**
 * @brief init is called at the beginning of the simulation
 */
void DrvStdMemory::init(unsigned int phase) {
    mem_->init(phase);
}

/**
 * @brief setup is called during setup phase
 */
void DrvStdMemory::setup() {
    mem_->setup();
    to_native_meta_data_.init(this);
}

/**
 * @brief translate a pgas pointer to a native pointer
 */
void
DrvStdMemory::toNativePointerDRAM(DrvAPI::DrvAPIAddress addr, const DrvAPI::DrvAPIAddressInfo &decode, void **ptr, size_t *size) {
    DrvAPI::DrvAPISysConfig cfg = core_->sysConfig().config();
    uint32_t interleave = cfg.pxnDRAMInterleaveSize();
    int pxn = decode.pxn();
    std::vector<record_type> &dram_mcs = to_native_meta_data_.dram_mcs[pxn];
    uint64_t dram_offset = decode.offset();
    uint64_t bank, offset;
    std::tie(bank, offset)
        = to_native_meta_data_
        .dram_interleave_decode
        .getBankOffset(dram_offset);

    DrvAPI::DrvAPIAddress start, stop;
    SST::MemHierarchy::MemController *mc;
    std::tie(start, stop, mc) = dram_mcs[bank];
    uint64_t laddr = mc->translateToLocal(addr);
    auto *backing = dynamic_cast<SST::MemHierarchy::Backend::BackingMMAP*>
        (mc->backing_);
    if (!backing) {
        output_.fatal(CALL_INFO, -1, "L2SP backing is not a MMAP\n");
    }
    uint8_t *bptr = &backing->m_buffer[laddr];
    *ptr = bptr;
    *size = interleave - offset;
}

/**
 * @brief translate a pgas pointer to a native pointer
 */
void
DrvStdMemory::toNativePointerL2SP(DrvAPI::DrvAPIAddress addr, const DrvAPI::DrvAPIAddressInfo &decode, void **ptr, size_t *size) {
    DrvAPI::DrvAPISysConfig cfg = core_->sysConfig().config();
    uint32_t interleave = cfg.podL2SPInterleaveSize();
    int pxn = decode.pxn();
    int pod = decode.pod();
    std::vector<record_type> &l2sp_mcs = to_native_meta_data_.l2sp_mcs[pxn][pod];

    uint64_t l2_offset = decode.offset();
    uint64_t bank, offset;
    std::tie(bank, offset)
        = to_native_meta_data_
        .l2sp_interleave_decode
        .getBankOffset(l2_offset);


    DrvAPI::DrvAPIAddress start, stop;
    SST::MemHierarchy::MemController *mc;
    std::tie(start, stop, mc) = l2sp_mcs[bank];
    uint64_t laddr = mc->translateToLocal(addr);
    auto *backing = dynamic_cast<SST::MemHierarchy::Backend::BackingMMAP*>
        (mc->backing_);
    if (!backing) {
        output_.fatal(CALL_INFO, -1, "L2SP backing is not a MMAP\n");
    }
    uint8_t *bptr = &backing->m_buffer[laddr];
    *ptr = bptr;
    *size = interleave - offset;
}

/**
 * @brief translate a pgas pointer to a native pointer
 */
void
DrvStdMemory::toNativePointerL1SP(DrvAPI::DrvAPIAddress addr, const DrvAPI::DrvAPIAddressInfo &decode, void **ptr, size_t *size) {
    int pxn = decode.pxn();
    int pod = decode.pod();
    int core = decode.core();
    // todo: we might not need a for loop here
    DrvAPI::DrvAPIAddress start, end;
    SST::MemHierarchy::MemController *mc;
    std::tie(start, end, mc) = to_native_meta_data_.l1sp_mcs[pxn][pod][core];
    if (start <= addr && addr < end) {
        uint64_t laddr = mc->translateToLocal(addr);
        auto *backing = dynamic_cast<SST::MemHierarchy::Backend::BackingMMAP*>
            (mc->backing_);
        if (!backing) {
            output_.fatal(CALL_INFO, -1, "Backing is not MMAP\n");
        }
        uint8_t *bptr = &backing->m_buffer[laddr];
        *ptr = bptr;
        *size = backing->m_size - laddr;
        return;
    }
    output_.fatal(CALL_INFO, -1, "Address 0x%lx not found in L1SP\n", addr);
}

/**
 * @brief translate a pgas pointer to a native pointer
 */
void
DrvStdMemory::toNativePointer(DrvAPI::DrvAPIAddress paddr, void **ptr, size_t *size) {
    DrvAPI::DrvAPIAddressInfo decode = core_->decoder().decode(paddr);
    if (decode.is_dram()) {
        return toNativePointerDRAM(paddr, decode, ptr, size);
    } else if (decode.is_l2sp()) {
        return toNativePointerL2SP(paddr, decode, ptr, size);
    } else if (decode.is_l1sp()) {
        return toNativePointerL1SP(paddr, decode, ptr, size);
    } else {
        output_.fatal(CALL_INFO, -1, "Unknown address type\n");
    }
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
    DrvAPI::DrvAPIAddressInfo paddr = core->decoder().decode(mem_req->getAddress());
    bool noncacheable = !paddr.is_dram();
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
        if (noncacheable) req->setNoncacheable();
        // add statistic
        core->addStoreStat(paddr, thread);
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
        if (noncacheable) req->setNoncacheable();
        core->addLoadStat(paddr, thread);
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
        core->addAtomicStat(paddr, thread);
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

    auto flush_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIFlushLine>(mem_req);
    if (flush_req) {
        return sendFlushLine(core, thread, flush_req);
    }

    auto inv_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIInvLine>(mem_req);
    if (inv_req) {
        return sendInvalidateLine(core, thread, inv_req);
    }
    
    // fatally error if we don't know the request type
    if (!(write_req || read_req || to_native_req || atomic_req || inv_req || flush_req)) {
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
        // trace
        DrvAPI::DrvAPIAddressInfo paddr = core_->decoder().decode(write_rsp->pAddr);
        if (paddr.pxn() != (int64_t)core_->pxn_) {
            core_->traceRemotePxnMem(DrvCore::TRACE_REMOTE_PXN_STORE, "write_rsp", paddr, thread);
        }
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
        // trace
        DrvAPI::DrvAPIAddressInfo paddr = core_->decoder().decode(read_rsp->pAddr);
        if (paddr.pxn() != (int64_t)core_->pxn_) {
            core_->traceRemotePxnMem(DrvCore::TRACE_REMOTE_PXN_LOAD, "read_rsp", paddr, thread);
        }
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
            DrvAPI::DrvAPIAddressInfo paddr = core_->decoder().decode(areq_data->pAddr);
            if (paddr.pxn() != (int64_t)core_->pxn_) {
                core_->traceRemotePxnMem(DrvCore::TRACE_REMOTE_PXN_ATOMIC, "atomic_rsp", paddr, thread);
            }
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


    auto flush_rsp = dynamic_cast<StandardMem::FlushLineResp*>(req);
    if (flush_rsp) {
        output_.verbose(CALL_INFO, 10, DrvMemory::VERBOSE_REQ,
                        "Received flush response\n");
        DrvThread *thread = core_->getThread(flush_rsp->tid);
        auto flush_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIFlushLine>(thread->getAPIThread().getState());
        if (flush_req) {
            flush_req->complete();
        } else {
            output_.fatal(CALL_INFO, -1, "Failed to find memory request for tid=%" PRIu32 "\n", flush_rsp->tid);
        }
    }

    auto inv_rsp = dynamic_cast<StandardMem::InvLineResp*>(req);
    if (inv_rsp) {
        output_.verbose(CALL_INFO, 10, DrvMemory::VERBOSE_REQ,
                        "Received inv response\n");
        DrvThread *thread = core_->getThread(inv_rsp->tid);
        auto inv_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIInvLine>(thread->getAPIThread().getState());
        if (inv_req) {
            inv_req->complete();
        } else {
            output_.fatal(CALL_INFO, -1, "Failed to find memory request for tid=%" PRIu32 "\n", inv_rsp->tid);
        }
    }

    // fatally error if we don't know the response type
    if (!(write_rsp || read_rsp || (custom_rsp && areq_data) || write_req || flush_rsp || inv_rsp)) {
        output_.fatal(CALL_INFO, -1, "Unknown memory response type: %s\n", req->getString().c_str());
    }

    // must delete the request
    delete req;    
    core_->assertCoreOn();
}

void DrvStdMemory::sendFlushLine(DrvCore *core, DrvThread *thread, const std::shared_ptr<DrvAPI::DrvAPIFlushLine> &flush) {
    DrvAPI::DrvAPIAddress paddr = flush->getAddress();
    DrvAPI::DrvAPIAddressInfo info = core->decoder().decode(paddr).set_absolute(true);        
    StandardMem::FlushLine *req = new StandardMem::FlushLine(core_->decoder().encode(info), flush->line_);
    req->tid = core->getThreadID(thread);
    mem_->send(req);
}

void DrvStdMemory::sendInvalidateLine(DrvCore *core, DrvThread *thread, const std::shared_ptr<DrvAPI::DrvAPIInvLine> &inv_req) {
    DrvAPI::DrvAPIAddress paddr = inv_req->getAddress();
    DrvAPI::DrvAPIAddressInfo info = core->decoder().decode(paddr).set_absolute(true);        
    StandardMem::InvLine *req = new StandardMem::InvLine(core_->decoder().encode(info), inv_req->line_);
    req->tid = core->getThreadID(thread);
    mem_->send(req);
}
