// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

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
#include "DrvAPIAddress.hpp"
#include "DrvAPIAddressMap.hpp"
#include "DrvStats.hpp"
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
        {"sp", "[Core Value. ...]", ""},
        {"clock", "Clock rate in Hz", "1GHz"},
        {"num_harts", "Number of harts", "1"},
        {"core", "Core ID", "0"},
        {"pod", "Pod ID", "0"},
        {"pxn", "PXN ID", "0"},
        /* debugging */
        {"verbose", "Verbosity of output", "0"},
        {"debug_memory", "Debug memory requests", "0"},
        {"debug_idle", "Debug idle cycles", "0"},
        {"debug_requests", "Debug requests", "0"},
        {"debug_responses", "Debug responses", "0"},
        {"debug_syscalls", "Debug system calls", "0"},
    )
    // DOCUMENT SUBCOMPONENTS
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"memory", "Interface to a memory hierarchy", "SST::Interfaces::StandardMem"},
    )

    // DOCUMENT STATISTICS
    /* unfortunately the macro doesn't work with including "InstructionTable.h" */
    static const std::vector<SST::ElementInfoStatistic>& ELI_getStatistics()
    {
#define DEFINSTR(mnemonic, ...)                                         \
        {#mnemonic "_count", "Number of " #mnemonic " instructions", "instructions", 2},

#define DEFINE_DRV_STAT(name, desc, unit, load_level)                   \
        {#name, desc, unit, load_level},

        static std::vector<SST::ElementInfoStatistic> var    = {
#include <InstructionTable.h>
#include <DrvStatsTable.hpp>
        };

#undef DEFINSTR
#undef DEFINE_DRV_STAT
        auto parent = SST::ELI::InfoStats<
            std::conditional<(__EliDerivedLevel > __EliBaseLevel), __LocalEliBase, __ParentEliBase>::type>::get();
        SST::ELI::combineEliInfo(var, parent);
        return var;
    }


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

    static constexpr uint32_t DEBUG_MEMORY   = (1<< 0); //!< debug memory requests
    static constexpr uint32_t DEBUG_IDLE     = (1<< 1); //!< debug idle cycles
    static constexpr uint32_t DEBUG_SYSCALLS = (1<< 2); //!< debug system calls
    static constexpr uint32_t DEBUG_REQ      = (1<<30); //!< debug messages we expect to see when receiving requests
    static constexpr uint32_t DEBUG_RSP      = (1<<29); //!< debug messages we expect to see when receiving responses
    
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
#ifdef SST_RISCV_CORE_PROFILE_INSTRUCTIONS
        auto it = pchist_.find(hart.pc());
        if (it == pchist_.end()) {
            pchist_[hart.pc()] = 1;
        } else {
            it->second++;
        }
#endif
    }

    /**
     * get system info
     */
    DrvAPI::DrvAPISysConfig sys() const { return sys_config_.config(); }

    /**
     * get the max write request size
     */
    size_t getMaxReqSize() const { return sys().numNWObufDwords() * sizeof(uint64_t); }

    /**
     * decode a virtual address to a physical address
     */
    DrvAPI::DrvAPIPAddress toPhysicalAddress(uint64_t addr) const;

    /**
     * is local l1sp for purpose of stats
     */
    bool isPAddressLocalL1SP(DrvAPI::DrvAPIPAddress addr) const {
        return addr.type() == DrvAPI::DrvAPIPAddress::TYPE_L1SP
            && addr.pxn() == static_cast<uint64_t>(pxn_)
            && addr.pod() == static_cast<uint64_t>(pod_)
            && addr.core_y() == static_cast<uint64_t>(DrvAPI::coreYFromId(core_))
            && addr.core_x() == static_cast<uint64_t>(DrvAPI::coreXFromId(core_));
    }

    /**
     * is remote l1sp for purpose of stats
     */
    bool isPAddressRemoteL1SP(DrvAPI::DrvAPIPAddress addr) const {
        return addr.type() == DrvAPI::DrvAPIPAddress::TYPE_L1SP
            && addr.pxn() == static_cast<uint64_t>(pxn_)
            && addr.pod() == static_cast<uint64_t>(pod_)
            && (   addr.core_y() != static_cast<uint64_t>(DrvAPI::coreYFromId(core_))
                || addr.core_x() != static_cast<uint64_t>(DrvAPI::coreXFromId(core_)));
    }

    /**
     * is remote pxn memory for purpose of stats
     */
    bool isPAddressRemotePXN(DrvAPI::DrvAPIPAddress addr) const {
        return addr.pxn() != static_cast<uint64_t>(pxn_);
    }

    /**
     * is  l2sp for purpose of stats
     */
    bool isPAddressL2SP(DrvAPI::DrvAPIPAddress addr) const {
        return addr.type() == DrvAPI::DrvAPIPAddress::TYPE_L2SP
            && addr.pxn() == static_cast<uint64_t>(pxn_)
            && addr.pod() == static_cast<uint64_t>(pod_);
    }

    /**
     * is  dram for purpose of stats
     */
    bool isPAddressDRAM(DrvAPI::DrvAPIPAddress addr) const {
        return addr.type() == DrvAPI::DrvAPIPAddress::TYPE_DRAM
            && addr.pxn() == static_cast<uint64_t>(pxn_);
    }

    /**
     * add load statistic
     */
    void addLoadStat(DrvAPI::DrvAPIPAddress addr) const {
        if (isPAddressLocalL1SP(addr))  drv_stats_[LOAD_LOCAL_L1SP]->addData(1);
        if (isPAddressRemoteL1SP(addr)) drv_stats_[LOAD_REMOTE_L1SP]->addData(1);
        if (isPAddressRemotePXN(addr))  drv_stats_[LOAD_REMOTE_PXN]->addData(1);
        if (isPAddressL2SP(addr))       drv_stats_[LOAD_L2SP]->addData(1);
        if (isPAddressDRAM(addr))       drv_stats_[LOAD_DRAM]->addData(1);
    }

    /**
     * add store statistic
     */
    void addStoreStat(DrvAPI::DrvAPIPAddress addr) const {
        if (isPAddressLocalL1SP(addr))  drv_stats_[STORE_LOCAL_L1SP]->addData(1);
        if (isPAddressRemoteL1SP(addr)) drv_stats_[STORE_REMOTE_L1SP]->addData(1);
        if (isPAddressRemotePXN(addr))  drv_stats_[STORE_REMOTE_PXN]->addData(1);
        if (isPAddressL2SP(addr))       drv_stats_[STORE_L2SP]->addData(1);
        if (isPAddressDRAM(addr))       drv_stats_[STORE_DRAM]->addData(1);
    }

    /**
     * add atomic statistic
     */
    void addAtomicStat(DrvAPI::DrvAPIPAddress addr) const {
        if (isPAddressLocalL1SP(addr))  drv_stats_[ATOMIC_LOCAL_L1SP]->addData(1);
        if (isPAddressRemoteL1SP(addr)) drv_stats_[ATOMIC_REMOTE_L1SP]->addData(1);
        if (isPAddressRemotePXN(addr))  drv_stats_[ATOMIC_REMOTE_PXN]->addData(1);
        if (isPAddressL2SP(addr))       drv_stats_[ATOMIC_L2SP]->addData(1);
        if (isPAddressDRAM(addr))       drv_stats_[ATOMIC_DRAM]->addData(1);
    }

    /**
     * configure statistics
     */
    void configureStatistics(Params &params);

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
    std::vector<Statistic<uint64_t>*> instruction_counts_; //!< instruction counts
    std::vector<Statistic<uint64_t>*> drv_stats_; //!< common drv stats
};


}
}

