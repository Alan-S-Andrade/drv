#include "SSTRISCVCore.hpp"
#include "SSTRISCVSimulator.hpp"

namespace SST {
namespace Drv {

void RISCVCore::configureClock(Params &params) {
    std::string clock = params.find<std::string>("clock", "1GHz");
    clocktc_ = registerClock(clock, new Clock::Handler<RISCVCore>(this, &RISCVCore::tick));
}

void RISCVCore::configureOuptut(Params& params) {
    int verbose_level = params.find<int>("verbose", 0);
    uint32_t verbose_mask = 0;
    output_.init("SSTRISCVCore[@p:@l]: ", verbose_level, verbose_mask, Output::STDOUT);
}

void RISCVCore::configureHarts(Params &params) {
    size_t num_harts = params.find<size_t>("num_harts", 1);
    for (size_t i = 0; i < num_harts; i++) {
        harts_.push_back(RISCVSimHart{});
    }
    std::vector<KeyValue<int, uint64_t>> sps;
    params.find_array<KeyValue<int, uint64_t>>("sp", sps);
    output_.verbose(CALL_INFO, 1, 0, "Configuring sp for %lu harts\n", sps.size());
    for (auto &sp : sps) {
        output_.verbose(CALL_INFO, 1, 0, "Hart %d sp = 0x%lx\n", sp.key, sp.value);
        harts_[sp.key].sp() = sp.value;
    }
}

void RISCVCore::configureICache(Params &params) {
    std::string program = params.find<std::string>("program", "");
    if (program.empty()) {
        output_.fatal(CALL_INFO, -1, "No program specified\n");
    }
    icache_ = new ICacheBacking(program.c_str());
    load_program_ = params.find<bool>("load", false);
}

void RISCVCore::configureMemory(Params &params) {
    mem_ = loadUserSubComponent<Interfaces::StandardMem>
        ("memory", ComponentInfo::SHARE_NONE, clocktc_,
         new Interfaces::StandardMem::Handler<RISCVCore>(this, &RISCVCore::handleMemEvent));

}

void RISCVCore::configureSimulator(Params &params) {
    sim_ = new RISCVSimulator(this);
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
    configureMemory(params);
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
}

/* destructor */
RISCVCore::~RISCVCore() {
    delete icache_;
    delete sim_;
}

/* load program segment */
void RISCVCore::loadProgramSegment(Elf64_Phdr* phdr) {
    output_.verbose(CALL_INFO, 1, 0, "Loading program segment: 0x%lx\n", phdr->p_vaddr);
    std::vector<uint8_t> data(phdr->p_memsz, 0);
    memcpy(&data[0], icache_->segment(phdr), phdr->p_filesz);
    // issue a memory request
    using Write = Interfaces::StandardMem::Write;
    Write *wr = new Write(phdr->p_vaddr, data.size(), data, true);
    mem_->send(wr);
}

/* load program */
void RISCVCore::loadProgram() {
    for (int pidx = 0; pidx < icache_->ehdr()->e_phnum; pidx++) {
        Elf64_Phdr *phdr = icache_->phdr(pidx);
        if (phdr->p_type == PT_LOAD &&
            ! (phdr->p_flags & PF_X)) {
            loadProgramSegment(phdr);
        }
    }
}

/* init */
void RISCVCore::init(unsigned int phase) {
    for (RISCVSimHart &hart : harts_) {
        hart.pc() = icache_->getStartAddr();
        hart.ready() = true;
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
    // load program data
    if (load_program_) {
        output_.verbose(CALL_INFO, 1, 0, "Loading program\n");
        loadProgram();
        output_.verbose(CALL_INFO, 1, 0, "Program loaded\n");
    }
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
        output_.verbose(CALL_INFO, 3, 0, "0x%lx: %lu\n", pc.first, pc.second);
    }
    auto stdmem = dynamic_cast<Interfaces::StandardMem*>(mem_);
    if (stdmem) {
        stdmem->finish();
    }
}

/* handle memory event */
void RISCVCore::handleMemEvent(RISCVCore::Request *req) {
    output_.verbose(CALL_INFO, 50, 0, "Received memory request\n");
    int tid = -1;

    auto *rd_rsp = dynamic_cast<Interfaces::StandardMem::ReadResp*>(req);
    if (rd_rsp) {
        tid = rd_rsp->tid;
    }

    auto *wr_rsp = dynamic_cast<Interfaces::StandardMem::WriteResp*>(req);
    if (wr_rsp) {
        tid = wr_rsp->tid;
    }

    auto *custom_rsp = dynamic_cast<Interfaces::StandardMem::CustomResp*>(req);
    if (custom_rsp) {
        tid = custom_rsp->tid;
    }

    if (!rd_rsp && !wr_rsp && !custom_rsp) {
        output_.fatal(CALL_INFO, -1, "Unknown memory request type\n");
    }

    auto it = rsp_handlers_.find(tid);
    if (it == rsp_handlers_.end()) {
        output_.fatal(CALL_INFO, -1, "Received memory request for unknown hart\n");
    }
    it->second(req);
}

/* select the next hart to execute */
int RISCVCore::selectNextHart() {
    int start = last_hart_ + 1;
    int stop = start + harts_.size();
    for (int h = start; h < stop; h++) {
        int hart_id = h % harts_.size();
        RISCVSimHart &hart = harts_[hart_id];
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
    if (hart_id != NO_HART) {
        uint64_t pc = harts_[hart_id].pc();
        uint64_t inst = icache_->read(pc);
        RISCVInstruction *i = decoder_.decode(inst);
        output_.verbose(CALL_INFO, 100, 0, "Ticking hart %2d: pc = 0x%016lx, instr = 0x%08x (%s)\n"
                        ,hart_id
                        ,pc
                        ,inst
                        ,i->getMnemonic()
                        );
        profileInstruction(harts_[hart_id], *i);
        sim_->visit(harts_[hart_id], *i);
        delete i;
    } else {
        output_.verbose(CALL_INFO, 100, 0, "No harts ready to execute\n");
    }

    if (shouldExit())
        primaryComponentOKToEndSim();
    
    return shouldExit();
}

/**
 * issue a memory request
 */
void RISCVCore::issueMemoryRequest(Request *req, int tid, ICompletionHandler &handler) {
    output_.verbose(CALL_INFO, 50, 0, "Issuing memory request\n");
    // TODO: check if tid is valid
    rsp_handlers_[tid] = handler;
    mem_->send(req);
}
}
}
