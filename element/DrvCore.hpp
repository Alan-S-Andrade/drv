// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#pragma once
#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/event.h>
#include <memory>
#include "DrvEvent.hpp"
#include "DrvMemory.hpp"
#include "DrvThread.hpp"
#include "DrvSystem.hpp"
#include "DrvSysConfig.hpp"
#include "DrvAPIMain.hpp"
#include "DrvStats.hpp"
#include <DrvAPI.hpp>
namespace SST {
namespace Drv {
/**
 * A Core
 */
class DrvCore : public SST::Component {
public:
  // REGISTER THIS COMPONENT INTO THE ELEMENT LIBRARY
  SST_ELI_REGISTER_COMPONENT(
      DrvCore,
      "Drv",
      "DrvCore",
      SST_ELI_ELEMENT_VERSION(1,0,0),
      "Drv Core",
      COMPONENT_CATEGORY_UNCATEGORIZED
  )
  // Document the parameters that this component accepts
  SST_ELI_DOCUMENT_PARAMS(
      /* input */
      {"executable", "Path to user program", ""},
      {"argv","List of arguments for program", ""},
      /* system config */
      DRV_SYS_CONFIG_PARAMETERS
      /* core config */
      {"threads", "Number of threads on this core", "1"},
      {"clock", "Clock rate of core", "125MHz"},
      {"max_idle", "Max idle cycles before we unregister the clock", "1000000"},
      {"id", "ID for the core", "0"},
      {"pod", "Pod ID of this core", "0"},
      {"pxn", "PXN ID of this core", "0"},
      {"stack_in_l1sp", "Use modeled memory backing store for stack", "0"},
      {"dram_base", "Base address of DRAM", "0x80000000"},
      {"dram_size", "Size of DRAM", "0x100000000"},
      {"l1sp_base", "Base address of L1SP", "0x00000000"},
      {"l1sp_size", "Size of L1SP", "0x00001000"},
      /* debug flags */
      {"verbose", "Verbosity of logging", "0"},
      {"debug_init", "Print debug messages during initialization", "False"},
      {"debug_clock", "Print debug messages we expect to see during clock ticks", "False"},
      {"debug_requests", "Print debug messages we expect to see during request events", "False"},
      {"debug_responses", "Print debug messages we expect to see during response events", "False"},
      {"debug_loopback", "Print debug messages we expect to see during loopback events", "False"},
      {"trace_remote_pxn", "Trace all requests to remote pxn", "false"},
      {"trace_remote_pxn_load", "Trace loads to remote pxn", "false"},
      {"trace_remote_pxn_store", "Trace loads to remote pxn", "false"},
      {"trace_remote_pxn_atomic", "Trace loads to remote pxn", "false"},
  )
  // Document the ports that this component accepts
  SST_ELI_DOCUMENT_PORTS(
      {"loopback", "A loopback link", {"Drv.DrvEvent", ""}},
  )

  // Document the subcomponents that this component has
  SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
      {"memory", "Interface to memory hierarchy", "Drv::DrvMemory"},
  )

  // DOCUMENT STATISTICS
  /* unfortunately the macro doesn't work with including "DrvStatsTable.hpp" */
  static const std::vector<SST::ElementInfoStatistic>& ELI_getStatistics()
    {
#define DEFINE_DRV_STAT(name, desc, unit, load_level)   \
        {#name, desc, unit, load_level},

        static std::vector<SST::ElementInfoStatistic> var    = {
#include <DrvStatsTable.hpp>
        };

#undef DEFINE_DRV_STAT
        auto parent = SST::ELI::InfoStats<
            std::conditional<(__EliDerivedLevel > __EliBaseLevel), __LocalEliBase, __ParentEliBase>::type>::get();
        SST::ELI::combineEliInfo(var, parent);
        return var;
    }


  /**
   * constructor
   * @param[in] id The component id.
   * @param[in] params Parameters for this component.
   */
  DrvCore(SST::ComponentId_t id, SST::Params& params);

  /** destructor */
  ~DrvCore();

  /**
   * configure the executable
   * @param[in] params Parameters to this component.
   */
  void configureExecutable(SST::Params &params);

  /**
   * close the executable
   */
  void closeExecutable();
  
  /**
   * configure output logging
   * @param[in] params Parameters to this component.
   */
  void configureOutput(SST::Params &params);

  /**
   * configure output for tracing
   * param[in] params Parameters to this component.
   */
  void configureTrace(SST::Params &params);

  /**
   * configure clock
   * @param[in] params Parameters to this component.
   */
  void configureClock(SST::Params &params);

  /**
   * configure threads on the core
   * @param[in] params  Parameters to this component.
   */
  void configureThreads(SST::Params &params);

  /**
   * configure one thread
   * @param[in] thread A thread to initialize.
   * @param[in[ threads How many threads will be initialized.
   */
  void configureThread(int thread, int threads);  

  /**
   * configure memory
   * @param[in] params Parameters to this component.
   */
  void configureMemory(SST::Params &params);

  /**
   * configure other links
   * @param[in] params Parameters to this component.
   */
  void configureOtherLinks(SST::Params &params);

  /**
   * configure sysconfig
   * @param[in] params Parameters to this component.
   */
  void configureSysConfig(SST::Params &params);

  /**
   * configure statistics
   */
  void configureStatistics(Params &params);

  /**
   * select a ready thread
   */
  int selectReadyThread();
    
  /**
   * execute one ready thread   
   */
  void executeReadyThread();

  /**
   * parse the command line arguments
   */
  void parseArgv(SST::Params &params);
  
  /**
   * return true if simulation should finish
   */
  bool allDone();

  /**
   * clock tick handler
   * @param[in] cycle The current cycle number
   */
  virtual bool clockTick(SST::Cycle_t);

  /**
   * handle a loopback event
   * @param[in] event The event to handle
   */
  void handleLoopback(SST::Event *event);

  /**
   * set the current thread context
   */
  void setThreadContext(DrvThread* thread) {
    set_thread_context_(&thread->getAPIThread());
  }

  /**
   * return the output stream of this core
   */
  std::unique_ptr<SST::Output>& output() {
    return output_;
  }

  /**
   * handle a thread state after the the thread has yielded back to the simulator
   * @param[in] thread The thread to handle
   */
  void handleThreadStateAfterYield(DrvThread *thread);
  
  static constexpr uint32_t DEBUG_INIT     = (1<< 0); //!< debug messages during initialization
  static constexpr uint32_t DEBUG_CLK      = (1<<31); //!< debug messages we expect to see during clock ticks
  static constexpr uint32_t DEBUG_REQ      = (1<<30); //!< debug messages we expect to see when receiving requests
  static constexpr uint32_t DEBUG_RSP      = (1<<29); //!< debug messages we expect to see when receiving responses
  static constexpr uint32_t DEBUG_LOOPBACK = (1<<28); //!< debug messages we expect to see when receiving loopback events

  static constexpr uint32_t TRACE_REMOTE_PXN_STORE = (1<< 0); //!< trace remote store events
  static constexpr uint32_t TRACE_REMOTE_PXN_LOAD  = (1<< 1); //!< trace remote load events
  static constexpr uint32_t TRACE_REMOTE_PXN_ATOMIC= (1<< 2); //!< trace remote atomic events
  static constexpr uint32_t TRACE_REMOTE_PXN_MEMORY = (TRACE_REMOTE_PXN_STORE | TRACE_REMOTE_PXN_LOAD | TRACE_REMOTE_PXN_ATOMIC); //!< trace remote memory events
private:
  std::unique_ptr<SST::Output> trace_; //!< for tracing

public:
  void traceRemotePxnMem(uint32_t trace_mask, const char* opname,
                         DrvAPI::DrvAPIPAddress paddr) const {
      trace_->verbose(CALL_INFO, 0, trace_mask
                      ,"OP=%s:SRC_PXN=%d:SRC_POD=%d:SRC_CORE=%d:DST_PXN=%d:ADDR=%s\n"
                      ,opname
                      ,pxn_
                      ,pod_
                      ,id_
                      ,(int)paddr.pxn()
                      ,paddr.to_string().c_str()
                      );
  }

  /**
   * initialize the component
   */
  void init(unsigned int phase) override;

  /**
   * setup the component
   */
  void setup() override;

  /**
   * start the threads
   */
  void startThreads();

  /**
   * finish the component
   */
  void finish() override;

  /**
   * return the id of a thread
   */
  int getThreadID(DrvThread *thread) {
    size_t tid = thread - &threads_[0];
    assert(tid < threads_.size());
    return static_cast<int>(tid);
  }

  /**
   * return pointer to thread
   */
  DrvThread* getThread(int tid) {
    assert(tid >= 0 && static_cast<size_t>(tid) < threads_.size());
    return &threads_[tid];
  }

  /**
   * return the number of threads on this core
   */
  int numThreads() const {
     return static_cast<int>(threads_.size());
  }        

  /**
   * return the time converter for the clock
   */
  SST::TimeConverter* getClockTC() {
    return clocktc_;
  }

  /**
   * return true if we should unregister the clock
   */
  bool shouldUnregisterClock() {    
    return allDone() || (idle_cycles_ >= max_idle_cycles_);
  }

  /**
   * turn the core on if it's off
   */
  void assertCoreOn() {
    if (!core_on_) {
      core_on_ = true;
      output_->verbose(CALL_INFO, 2, DEBUG_RSP, "turning core on\n");
      reregister_cycle_ = system_callbacks_->getCycleCount();
      addStallCycleStat(reregister_cycle_ - unregister_cycle_);
      reregisterClock(clocktc_, new SST::Clock::Handler<DrvCore>(this, &DrvCore::clockTick));      
    }
  }

  /**
   * set the application system configuration
   */
  void setSysConfigApp() {
      DrvAPI::DrvAPISysConfig sys_cfg_app = sys_config_.config();
      set_sys_config_app_(&sys_cfg_app);
  }

    /**
     * is local l1sp for purpose of stats
     */
    bool isPAddressLocalL1SP(DrvAPI::DrvAPIPAddress addr) const {
        return addr.type() == DrvAPI::DrvAPIPAddress::TYPE_L1SP
            && addr.pxn() == static_cast<uint64_t>(pxn_)
            && addr.pod() == static_cast<uint64_t>(pod_)
            && addr.core_y() == static_cast<uint64_t>(DrvAPI::coreYFromId(id_))
            && addr.core_x() == static_cast<uint64_t>(DrvAPI::coreXFromId(id_));
    }

    /**
     * is remote l1sp for purpose of stats
     */
    bool isPAddressRemoteL1SP(DrvAPI::DrvAPIPAddress addr) const {
        return addr.type() == DrvAPI::DrvAPIPAddress::TYPE_L1SP
            && addr.pxn() == static_cast<uint64_t>(pxn_)
            && addr.pod() == static_cast<uint64_t>(pod_)
            && (   addr.core_y() != static_cast<uint64_t>(DrvAPI::coreYFromId(id_))
                || addr.core_x() != static_cast<uint64_t>(DrvAPI::coreXFromId(id_)));
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
        if (isPAddressL2SP(addr))       drv_stats_[LOAD_L2SP]->addData(1);
        if (isPAddressDRAM(addr))       drv_stats_[LOAD_DRAM]->addData(1);
        if (isPAddressRemotePXN(addr))  {
            traceRemotePxnMem(TRACE_REMOTE_PXN_LOAD, "read", addr);
            drv_stats_[LOAD_REMOTE_PXN]->addData(1);
        }
    }

    /**
     * add store statistic
     */
    void addStoreStat(DrvAPI::DrvAPIPAddress addr) const {
        if (isPAddressLocalL1SP(addr))  drv_stats_[STORE_LOCAL_L1SP]->addData(1);
        if (isPAddressRemoteL1SP(addr)) drv_stats_[STORE_REMOTE_L1SP]->addData(1);
        if (isPAddressL2SP(addr))       drv_stats_[STORE_L2SP]->addData(1);
        if (isPAddressDRAM(addr))       drv_stats_[STORE_DRAM]->addData(1);
        if (isPAddressRemotePXN(addr))  {
            traceRemotePxnMem(TRACE_REMOTE_PXN_STORE, "write", addr);
            drv_stats_[STORE_REMOTE_PXN]->addData(1);
        }
    }

    /**
     * add atomic statistic
     */
    void addAtomicStat(DrvAPI::DrvAPIPAddress addr) const {
        if (isPAddressLocalL1SP(addr))  drv_stats_[ATOMIC_LOCAL_L1SP]->addData(1);
        if (isPAddressRemoteL1SP(addr)) drv_stats_[ATOMIC_REMOTE_L1SP]->addData(1);
        if (isPAddressL2SP(addr))       drv_stats_[ATOMIC_L2SP]->addData(1);
        if (isPAddressDRAM(addr))       drv_stats_[ATOMIC_DRAM]->addData(1);
        if (isPAddressRemotePXN(addr))  {
            traceRemotePxnMem(TRACE_REMOTE_PXN_ATOMIC, "atomic", addr);
            drv_stats_[ATOMIC_REMOTE_PXN]->addData(1);
        }
    }

    void outputStatistics() {
        performGlobalStatisticOutput();
    }

    void addBusyCycleStat(uint64_t cycles) {
        drv_stats_[BUSY_CYCLES]->addData(cycles);
    }

    void addStallCycleStat(uint64_t cycles) {
        drv_stats_[STALL_CYCLES]->addData(cycles);
    }
private:  
  std::unique_ptr<SST::Output> output_; //!< for logging
  std::vector<DrvThread> threads_; //!< the threads on this core
  void *executable_; //!< the executable handle
  drv_api_main_t main_; //!< the main function in the executable
  drv_api_get_thread_context_t get_thread_context_; //!< the get_thread_context function in the executable
  drv_api_set_thread_context_t set_thread_context_; //!< the set_thread_context function in the executable
  DrvAPIGetSysConfig_t get_sys_config_app_; //!< the get_sys_config function in the executable
  DrvAPISetSysConfig_t set_sys_config_app_; //!< the set_sys_config function in the executable
  int done_; //!< number of threads that are done
  int last_thread_; //!< last thread that was executed
  std::vector<char*> argv_; //!< the command line arguments
  SST::Link *loopback_; //!< the loopback link
  uint64_t max_idle_cycles_; //!< maximum number of idle cycles
  uint64_t idle_cycles_; //!< number of idle cycles
  SimTime_t unregister_cycle_; //!< cycle when the clock handler was last unregistered
  SimTime_t reregister_cycle_; //!< cycle when the clock handler was last reregistered
  bool core_on_; //!< true if the core is on (clock handler is registered)  
  DrvSysConfig sys_config_; //!< system configuration
  bool stack_in_l1sp_ = false; //!< true if the stack is in L1SP backing store
  std::shared_ptr<DrvSystem> system_callbacks_ = nullptr; //!< the system callbacks
  std::vector<Statistic<uint64_t>*> drv_stats_; //!< the statistics

public:
  DrvMemory* memory_;  //!< the memory hierarchy
  SST::TimeConverter *clocktc_; //!< the clock time converter

  int id_; //!< the core id
  int pod_; //!< pod id of this core
  int pxn_; // !< pxn id of this core
};
}
}
