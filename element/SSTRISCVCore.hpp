#pragma once
#include <sstream>
#include <map>
#include <string>
#include <sst/core/component.h>
#include <sst/core/interfaces/stdMem.h>
#include <RISCVHart.hpp>
#include <RISCVInstruction.hpp>
#include <RISCVDecoder.hpp>
#include <RISCVInterpreter.hpp>
#include <ICacheBacking.hpp>
#include "SSTRISCVSimulator.hpp"
#include "SSTRISCVHart.hpp"
#include "DrvSysConfig.hpp"

namespace SST {
namespace Drv {

class RISCVCore : public SST::Component {
public:
    typedef Interfaces::StandardMem::Request Request;
    typedef std::function<void(Request *)> ICompletionHandler;
    
    // REGISTER THIS COMPONENT INTO THE ELEMENT LIBRARY
    SST_ELI_REGISTER_COMPONENT(
        RISCVCore,
        "Drv",
        "RISCVCore",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "RISCV Core",
        COMPONENT_CATEGORY_PROCESSOR
    )
    // DOCUMENT PARAMETERS
    SST_ELI_DOCUMENT_PARAMS(
        /* input */
        {"program", "Program to run", "/path/to/r64elf"},
        {"load", "Load program into memory", "0"},
        /* system config */
        DRV_SYS_CONFIG_PARAMETERS
        {"sp", "[Core Value. ...]"},
        {"clock", "Clock rate in Hz", "1GHz"},
        {"num_harts", "Number of harts", "1"},
        {"core", "Core ID", "0"},
        {"pod", "Pod ID", "0"},
        {"pxn", "PXN ID", "0"},
        /* debugging */
        {"verbose", "Verbosity of output", "0"},        
    )
    // DOCUMENT SUBCOMPONENTS
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"memory", "Interface to a memory hierarchy", "SST::Interfaces::StandardMem"},
    )

    /**
     * A key value pair
     */
    template <typename Key, typename Value>
    class KeyValue {
    public:
        KeyValue(std::string str) {
            std::stringstream ss(str);
            ss >> key >> value;
        }
        KeyValue(Key key, Value value) : key(key), value(value) {}
        Key   key;
        Value value;              
    };
    
    /**
     * Constructor for RISCVCore
     */
    RISCVCore(SST::ComponentId_t id, SST::Params& params);

    /**
     * Destructor for RISCVCore
     */
    ~RISCVCore();

    /**
     * Init the simulation
     */
    void init(unsigned int phase) override;
    
    /**
     * Setup the simulation
     */
    void setup() override;
    
    /**
     * Finish the simulation
     */
    void finish() override;

    /**
     * Load a program segment
     */
    void loadProgramSegment(Elf64_Phdr* phdr);

    /**
     * Load a program
     */
    void loadProgram();
    
    /**
     * configure output stream
     */
    void configureOuptut(Params& params);

    /**
     * configure harts
     */
    void configureHarts(Params &params);

    /**
     * configure icache
     */
    void configureICache(Params &params);

    /**
     * configure memory
     */
    void configureMemory(Params &params);

    /**
     * configure clock
     */
    void configureClock(Params &params);

    /**
     * configure simulator
     */
    void configureSimulator(Params &params);

    /**
     * configure system config
     */
    void configureSysConfig(Params &params);
    
    /**
     * clock tick
     */
    bool tick(Cycle_t cycle);

    /**
     * handle a memory event
     */
    void handleMemEvent(Interfaces::StandardMem::Request *req);
    
    /**
     * get the number of harts on this core
     */
    size_t numHarts() const { return harts_.size(); }


    int getHartId(RISCVSimHart &hart) const {
        return &hart - &harts_[0];
    }

    int getCoreId() const {
        return core_;
    }

    int getPodId() const {
        return pod_;
    }

    int getPXNId() const {
        return pxn_;
    }
    
    static constexpr int NO_HART = -1;
    /**
     * select the next hart to execute
     */
    int selectNextHart();

    /**
     * issue a memory request
     */
    void issueMemoryRequest(Request *req, int tid, ICompletionHandler &handler);

    /**
     * return true if we should exit
     */
    bool shouldExit() const {
        for (auto &hart : harts_) {
            if (!hart.exit())
                return false;
        }
        return true;
    }

    void profileInstruction(RISCVSimHart &hart, RISCVInstruction &instruction) {
        auto it = pchist_.find(hart.pc());
        if (it == pchist_.end()) {
            pchist_[hart.pc()] = 1;
        } else {
            it->second++;
        }
    }

    /**
     * get system info
     */
    DrvAPI::DrvAPISysConfig sys() const { return sys_config_.config(); }
    
    SST::Output output_; //!< output stream
    Interfaces::StandardMem *mem_; //!< memory interface
    RISCVSimulator *sim_; //!< simulator
    ICacheBacking *icache_; //!< icache
    RISCVDecoder decoder_; //!< decoder
    std::vector<RISCVSimHart> harts_; //!< harts
    std::map<int, ICompletionHandler> rsp_handlers_; //!< response handlers
    SST::TimeConverter *clocktc_; //!< the clock time converter
    int last_hart_; //!< last hart to execute
    bool load_program_; //!< load program
    DrvSysConfig sys_config_; //!< system configuration
    std::map<uint64_t, int64_t> pchist_; //!< program counter history
    int core_; //!< core id wrt pod
    int pod_;  //!< pod id wrt pxn
    int pxn_;  //!< pxn id wrt system
};


}
}

