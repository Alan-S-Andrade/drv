// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include "SSTRISCVCore.hpp"
#include "SSTRISCVSimulator.hpp"
#include <DrvAPIAddressMap.hpp>
#include <DrvAPIInfo.hpp>
namespace SST {
namespace Drv {

void RISCVCore::configureClock(Params &params) {
    std::string clock = params.find<std::string>("clock", "1GHz");
    clock_handler_ = new Clock::Handler<RISCVCore>(this, &RISCVCore::tick);
    clocktc_ = registerClock(clock, clock_handler_);
}

void RISCVCore::configureOuptut(Params& params) {
    int verbose_level = params.find<int>("verbose", 0);
    uint32_t verbose_mask = 0;
    if (params.find<bool>("debug_memory", false)) {
        verbose_mask |= DEBUG_MEMORY;
    }
    if (params.find<bool>("debug_idle", false)) {
        verbose_mask |= DEBUG_IDLE;
    }
    if (params.find<bool>("debug_requests", false)) {
        verbose_mask |= DEBUG_REQ;
    }
    if (params.find<bool>("debug_responses", false)) {
        verbose_mask |= DEBUG_RSP;
    }
    if (params.find<bool>("debug_syscalls", false)) {
        verbose_mask |= DEBUG_SYSCALLS;
    }
    if (params.find<bool>("debug_mmio", false)) {
        verbose_mask |= DEBUG_MMIO;
    }
    output_.init("SSTRISCVCore[@p:@l]: ", verbose_level, verbose_mask, Output::STDOUT);

    int isa_test = params.find<bool>("isa_test", 0);
    test_name_ = params.find<std::string>("test_name", "");
    isa_test_output_.init("", isa_test, 0, Output::STDOUT);
}

void RISCVCore::configureHarts(Params &params) {
    size_t num_harts = params.find<size_t>("num_harts", 1);
    std::cout << "Configuring " << num_harts << " harts\n";
    for (size_t i = 0; i < num_harts; i++) {
        harts_.push_back(RISCVSimHart{});
    }
    std::vector<KeyValue<int, uint64_t>> sps;
    params.find_array<KeyValue<int, uint64_t>>("sp", sps);
    std::cout << "sp" << sps.size() << "\n";
    if (sps.size() == 0) {
        output_.verbose(CALL_INFO, 1, 0, "Dividing L1SP stack amongst %lu harts\n", num_harts);
        output_.verbose(CALL_INFO, 1, 0, "L1SP base = 0x%lx coreL1SPSize = 0x%lx\n", l1spBase(), DrvAPI::coreL1SPSize());
        uint64_t stack_bytes = DrvAPI::coreL1SPSize() / num_harts;
        for (size_t i = 0; i < num_harts; i++) {
            uint64_t sp = l1spBase() + DrvAPI::coreL1SPSize() - (i * stack_bytes);
            output_.verbose(CALL_INFO, 1, 0, "Hart %lu sp = 0x%lx\n", i, sp);
            harts_[i].sp() = sp;
            harts_[i].spLow() = l1spBase() + (i * stack_bytes);
            harts_[i].spHigh() = l1spBase() + ((i + 1) * stack_bytes);
            output_.verbose(CALL_INFO, 1, 0, "Hart %lu spLow = 0x%lx spHigh = 0x%lx\n", i, harts_[i].spLow(), harts_[i].spHigh());
        }
    } else {
        output_.verbose(CALL_INFO, 1, 0, "Configuring sp for %lu harts\n", sps.size());
        for (auto &sp : sps) {
            output_.verbose(CALL_INFO, 1, 0, "Hart %d sp = 0x%lx\n", sp.key, sp.value);
            harts_[sp.key].sp() = sp.value;
        }
    }
}

void RISCVCore::configureICache(Params &params) {
    std::string program = params.find<std::string>("program", "");
    if (program.empty()) {
        output_.fatal(CALL_INFO, -1, "No program specified\n");
    }
    uint64_t icache_instructions = params.find<uint64_t>("icache_instructions", 1024);
    uint64_t icache_associativity = params.find<uint64_t>("icache_associativity", 1);
    auto *backing = new ICacheBacking(program.c_str());
    icache_ = new ICache(backing, icache_instructions, icache_associativity);
    load_program_ = params.find<bool>("load", false);
}

void RISCVCore::configureMemory(Params &params) {
    mem_ = loadUserSubComponent<Interfaces::StandardMem>
        ("memory", ComponentInfo::SHARE_NONE, clocktc_,
         new Interfaces::StandardMem::Handler<RISCVCore>(this, &RISCVCore::handleMemEvent));

    mmio_start_ = address_decoder_.this_cores_absolute_ctrl_base();
    mem_->setMemoryMappedAddressRegion(mmio_start_, 0x1000);
}

void RISCVCore::configureSimulator(Params &params) {
    sim_ = new RISCVSimulator(this);
}
    
void RISCVCore::configureSysConfig(Params &params) {
    sys_config_.init(params);
    core_ = params.find<int>("core", 0);
    pod_  = params.find<int>("pod", 0);
    pxn_  = params.find<int>("pxn", 0);
    address_decoder_ = DrvAPI::DrvAPIAddressDecoder(pxn_, pod_, core_, sys_config_.config());
}

void RISCVCore::configureStatistics(Params &params) {
    size_t num_harts = params.find<size_t>("num_harts", 1);
    thread_stats_.resize(num_harts);
    for (size_t hart = 0; hart < num_harts; hart++) {
        std::string subid = "hart_" + std::to_string(hart);
        ThreadStats &stats = thread_stats_[hart];
#define DEFINSTR(mnemonic, ...)                                         \
        {                                                               \
            std::string name = #mnemonic;                               \
            name += "_instruction";                                     \
            auto *stat = registerStatistic<uint64_t>(name, subid);      \
            stats.instruction_count.push_back(stat);                    \
        }        
#include <InstructionTable.h>
        stats.load_l1sp = registerStatistic<uint64_t>("load_l1sp", subid);
        stats.store_l1sp = registerStatistic<uint64_t>("store_l1sp", subid);
        stats.atomic_l1sp = registerStatistic<uint64_t>("atomic_l1sp", subid);
        stats.load_l2sp = registerStatistic<uint64_t>("load_l2sp", subid);
        stats.store_l2sp = registerStatistic<uint64_t>("store_l2sp", subid);
        stats.atomic_l2sp = registerStatistic<uint64_t>("atomic_l2sp", subid);
        stats.load_dram = registerStatistic<uint64_t>("load_dram", subid);
        stats.store_dram = registerStatistic<uint64_t>("store_dram", subid);
        stats.atomic_dram = registerStatistic<uint64_t>("atomic_dram", subid);
        stats.load_remote_pxn = registerStatistic<uint64_t>("load_remote_pxn", subid);
        stats.store_remote_pxn = registerStatistic<uint64_t>("store_remote_pxn", subid);
        stats.atomic_remote_pxn = registerStatistic<uint64_t>("atomic_remote_pxn", subid);
    }
    busy_cycles_ = registerStatistic<uint64_t>("busy_cycles");
    stall_cycles_ = registerStatistic<uint64_t>("stall_cycles");
    icache_miss_ = registerStatistic<uint64_t>("icache_miss");
}

void RISCVCore::configureLinks(Params &params) {
    loopback_ = configureSelfLink("loopback", new Event::Handler<RISCVCore>(this, &RISCVCore::handleLoopback));
    loopback_->addSendLatency(1, "ns");
    reset_time_ = params.find<uint64_t>("release_reset", 0);
}

/* constructor */
RISCVCore::RISCVCore(ComponentId_t id, Params& params)
    : Component(id)
    , mem_(nullptr)
    , icache_(nullptr)
    , harts_()
    , last_hart_(0) {
    configureOuptut(params);
    output_.verbose(CALL_INFO, 1, 0, "Configuring RISCVCore\n");
    configureClock(params);
    configureICache(params);
    configureSimulator(params);
    configureHarts(params);
    configureSysConfig(params);
    configureMemory(params);
    configureStatistics(params);
    configureLinks(params);
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
}

/* destructor */
RISCVCore::~RISCVCore() {
    delete icache_;
    delete sim_;
}

DrvAPI::DrvAPIAddressInfo RISCVCore::decodeAddress(uint64_t addr) const {
    return address_decoder_.decode(addr);
}

DrvAPI::DrvAPIAddress RISCVCore::l1spBase() const {
    DrvAPI::DrvAPIAddressInfo info;
    info.set_absolute(true)
        .set_core(core_)
        .set_pod(pod_)
        .set_pxn(pxn_)
        .set_l1sp()
        .set_offset(0);
    return address_decoder_.encode(info);
}

/* load program segment */
void RISCVCore::loadProgramSegment(Elf64_Phdr* phdr) {
    using Write = Interfaces::StandardMem::Write;
    using Addr = Interfaces::StandardMem::Addr;
    output_.verbose(CALL_INFO, 1, 0, "Loading program segment: (paddr = 0x%lx, vaddr = 0x%lx)\n"
                    , phdr->p_paddr
                    , phdr->p_vaddr);
    uint8_t *segp = static_cast<uint8_t*>(icache_->backing()->segment(phdr));
    size_t  segsz = phdr->p_filesz;
    size_t  reqsz = getMaxReqSize();

    auto decoded_phys_addr = decodeAddress(phdr->p_paddr);

    // skip if you are not the l2sp loader
    if (!load_program_ && !decoded_phys_addr.is_l1sp()) {
        return;
    }

    Addr segpaddr = address_decoder_.to_absolute(phdr->p_paddr);
    // write data
    for (;segsz > 0;) {
        size_t wrsz = std::min(reqsz, segsz);
        std::vector<uint8_t> data(wrsz, 0);
        memcpy(&data[0], segp, wrsz);
        Write *wr = new Write(segpaddr, wrsz, data, true);
        mem_->send(wr);
        segsz -= wrsz;
        segpaddr += wrsz;
        segp += wrsz;
    }
    // write zeros
    segsz = phdr->p_memsz - phdr->p_filesz;
    if (segsz > 0) {
        size_t wrsz = std::min(reqsz, segsz);
        std::vector<uint8_t> data(wrsz, 0);
        Write *wr = new Write(segpaddr, wrsz, data, true);
        mem_->send(wr);
        segsz -= wrsz;
        segpaddr += wrsz;
        segp += wrsz;
    }
}

/* load program */
void RISCVCore::loadProgram() {
    for (int pidx = 0; pidx < icache_->backing()->ehdr()->e_phnum; pidx++) {
        Elf64_Phdr *phdr = icache_->backing()->phdr(pidx);
        if (phdr->p_type == PT_LOAD) {
            loadProgramSegment(phdr);
        }
    }
}

/* init */
void RISCVCore::init(unsigned int phase) {
    for (RISCVSimHart &hart : harts_) {
        hart.resetPC() = icache_->backing()->getStartAddr();
        hart.reset() = true;
    }
    auto stdmem = dynamic_cast<Interfaces::StandardMem*>(mem_);
    if (stdmem) {
        stdmem->init(phase);
    }
}

/* setup */
void RISCVCore::setup() {
    auto stdmem = dynamic_cast<Interfaces::StandardMem*>(mem_);
    if (stdmem) {
        stdmem->setup();
    }
    output_.verbose(CALL_INFO, 1, 0, "memory: line size = %" PRIu64 "\n", stdmem->getLineSize());
    // load program data
    //output_.verbose(CALL_INFO, 1, 0, "Loading program\n");
    //loadProgram();
    //output_.verbose(CALL_INFO, 1, 0, "Program loaded\n");
}

/* finish */
void RISCVCore::finish() {
    for (size_t hart_id = 0; hart_id < harts_.size(); hart_id++) {
        RISCVHart &hart = harts_[hart_id];
        output_.verbose(CALL_INFO, 1, 0, "Hart %lu: hart: \n%s\n", hart_id, hart.to_string().c_str());
    }
    // dump pc histogram
    output_.verbose(CALL_INFO, 3, 0, "PC Histogram:\n");
    for (auto &pc : pchist_) {
        output_.verbose(CALL_INFO, 3, 0, "0x%08lx: %9lu\n", pc.first, pc.second);
    }
    output_.verbose(CALL_INFO, 3, 0, "End PC Histogram:\n");
    auto stdmem = dynamic_cast<Interfaces::StandardMem*>(mem_);
    if (stdmem) {
        stdmem->finish();
    }
    output_.verbose(CALL_INFO, 1, 0, "Finished\n");
}

/**
 * handle reset write
 */
void RISCVCore::handleResetWrite(uint64_t v) {
    output_.verbose(CALL_INFO, 0, DEBUG_MMIO, "PXN %d: POD %d: CORE %d: Received reset write request\n"
                    , pxn_, pod_, core_);
    if (v == 0) {
        // deassert reset
        for (RISCVSimHart &hart : harts_) {
            hart.reset() = false;
        }
        assertCoreOn();
    } else {
        // assert reset
        for (RISCVSimHart &hart : harts_) {
            hart.reset() = true;
        }
    }
}

/* handle mmio write request */
void RISCVCore::handleMMIOWrite(Interfaces::StandardMem::Write *write_req) {
    using namespace DrvAPI;
    auto *rsp = write_req->makeResponse();
    output_.verbose(CALL_INFO, 0, DEBUG_MMIO, "PXN %d: POD %d: CORE %d: Received MMIO write request\n"
                    , pxn_, pod_, core_);

    if (write_req->size != sizeof(uint64_t)) {
        output_.fatal(CALL_INFO, -1, "PXN %d: POD %d: CORE %d: MMIO write request size is not 8 bytes\n"
                      , pxn_, pod_, core_);
    }
    uint64_t v = *reinterpret_cast<uint64_t*>(&write_req->data[0]);

    DrvAPIAddressInfo paddr = address_decoder_.decode(write_req->pAddr);
    switch (paddr.offset()) {
    case DrvAPI::CTRL_CORE_RESET:
        handleResetWrite(v);
        break;
    default:
        output_.verbose(CALL_INFO, 0, DEBUG_MMIO, "PXN %d: POD %d: CORE %d: Unhandled MMIO write request\n"
                        , pxn_, pod_, core_);
    }
    mem_->send(rsp);
    delete write_req;
}

/* handle memory event */
void RISCVCore::handleMemEvent(RISCVCore::Request *req) {
    output_.verbose(CALL_INFO, 0, DEBUG_RSP, "Received memory response\n");
    int tid = -1;

    auto *rd_rsp = dynamic_cast<Interfaces::StandardMem::ReadResp*>(req);
    if (rd_rsp) {
        output_.verbose(CALL_INFO, 0, DEBUG_RSP, "Received read response\n");
        tid = rd_rsp->tid;
        assertCoreOn();
    }

    auto *wr_rsp = dynamic_cast<Interfaces::StandardMem::WriteResp*>(req);
    if (wr_rsp) {
        output_.verbose(CALL_INFO, 0, DEBUG_RSP, "Received write response\n");
        tid = wr_rsp->tid;
        assertCoreOn();
    }

    auto *custom_rsp = dynamic_cast<Interfaces::StandardMem::CustomResp*>(req);
    if (custom_rsp) {
        output_.verbose(CALL_INFO, 0, DEBUG_RSP, "Received custom response\n");
        tid = custom_rsp->tid;
        assertCoreOn();
    }

    auto *write_req = dynamic_cast<Interfaces::StandardMem::Write*>(req);
    if (write_req) {
        handleMMIOWrite(write_req);
    }

    if (rd_rsp || wr_rsp || custom_rsp) {
        auto it = rsp_handlers_.find(tid);
        if (it == rsp_handlers_.end()) {
            output_.fatal(CALL_INFO, -1, "Received memory response for unknown hart\n");
        }
        it->second(req);     
    } else if (!write_req) {
        output_.fatal(CALL_INFO, -1, "Unknown memory request type\n");
    }
}

/* select the next hart to execute */
int RISCVCore::selectNextHart() {
    int start = last_hart_ + 1;
    int stop = start + harts_.size();
    for (int h = start; h < stop; h++) {
        int hart_id = h % harts_.size();
        if (harts_[hart_id].ready()) {
            last_hart_ = hart_id;
            return hart_id;
        }
    }
    return NO_HART;
}

/* tick */
bool RISCVCore::tick(Cycle_t cycle) {
    int hart_id = selectNextHart();
    bool unregister = false;
    if (hart_id != NO_HART) {
        addBusyCycleStat(1);
        uint64_t pc = harts_[hart_id].pc();
        auto [hit, inst] = icache_->read(pc);
        if (!hit) {
            icache_miss_->addData(1);
        }
        RISCVInstruction *i = nullptr;
        try {
            i = decoder_.decode(inst);
        } catch (std::runtime_error &e) {
            std::stringstream ss;
            ss << "Failed to decode instruction at pc = 0x" << std::hex << pc << ": " << e.what();
            throw std::runtime_error(ss.str());
        }
        // std::stringstream ss;
        // // std::cout << inst << std::endl;
        // std::string str(i->getMnemonic());
        // ss << str << " ";
        // std::cout << ss.str() << std::endl;
        output_.verbose(CALL_INFO, 100, 0, "Ticking hart %2d: pc = 0x%016" PRIx64 ", instr = 0x%08" PRIx32" (%s)\n"
                        ,hart_id
                        ,pc
                        ,inst
                        ,i->getMnemonic()
                        );
        profileInstruction(harts_[hart_id], *i);
        auto &stats = thread_stats_[hart_id];
        stats.instruction_count[i->getInstructionId()]->addData(1);
        sim_->visit(harts_[hart_id], *i);
        delete i;
    } else {
        unregister = shouldUnregisterClock();
        if (unregister) {
            output_.verbose(CALL_INFO, 0, DEBUG_IDLE, "Unregistering clock\n");
            unregister_cycle_ = getCycleCount();
        }
        //addStallCycleStat(1);
        output_.verbose(CALL_INFO, 0, DEBUG_IDLE, "No harts ready to execute\n");
    }

    if (shouldExit())
        primaryComponentOKToEndSim();

    core_on_ = !unregister;
    return unregister ||  shouldExit();
}

/**
 * issue a memory request
 */
void RISCVCore::issueMemoryRequest(Request *req, int tid, ICompletionHandler &handler) {
    output_.verbose(CALL_INFO, 0, DEBUG_REQ, "Issuing memory request\n");
    // TODO: check if tid is valid
    std::cout << "issueMemoryRequest" << std::endl;
    rsp_handlers_[tid] = handler;
    mem_->send(req);
    std::cout << req << std::endl;
}

/**
 * put a hart to sleep
 */
void RISCVCore::putHartToSleep(RISCVSimHart &hart, uint64_t sleep_cycles) {
    auto *wake = new Wake();
    wake->hart() = getHartId(hart);
    loopback_->send(sleep_cycles, clocktc_, wake);
    hart.stalledSleep() = true;
}

/**
 * handle loopback event
 */
void RISCVCore::handleLoopback(Event *evt) {
    DeassertReset *deassert = dynamic_cast<DeassertReset*>(evt);
    if (deassert) {
        output_.verbose(CALL_INFO, 0, 0, "Received deassert reset event\n");
        for (auto &hart : harts_) {
            hart.reset() = false;
        }
        assertCoreOn();
    }
    Wake *wake = dynamic_cast<Wake*>(evt);
    if (wake) {
        output_.verbose(CALL_INFO, 1, 0, "Received wake event for hart %d\n"
                        ,wake->hart_);
        harts_[wake->hart_].stalledSleep() = false;
        assertCoreOn();
    }
    delete evt;
}

}
}

