#pragma once
#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/event.h>
#include <memory>
#include "DrvEvent.hpp"
#include "DrvMemory.hpp"
#include "DrvThread.hpp"
#include "DrvAPIMain.hpp"

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
      {"executable", "Path to user program"},
      {"argv","List of arguments for program", ""},
      /* system config */
      {"threads", "Number of threads on this core", "1"},
      {"clock", "Clock rate of core", "125MHz"},
      {"id", "ID for the core", "0"},
      {"dram_base", "Base address of DRAM", "0x80000000"},
      {"dram_size", "Size of DRAM", "0x100000000"},
      {"l1sp_base", "Base address of L1SP", "0x00000000"},
      {"l1sp_size", "Size of L1SP", "0x00001000"},
      /* debug flags */
      {"verbose", "Verbosity of logging", "0"},
      {"debug_init", "Print debug messages during initialization", "False"},
      {"debug_clock", "Print debug messages we expect to see during clock ticks", "False"},
      {"debug_requests", "Print debug messages we expect to see during request events"},
      {"debug_responses", "Print debug messages we expect to see during response events"},
  )
  // Document the ports that this component accepts
  SST_ELI_DOCUMENT_PORTS(
      {"mem_loopback", "A loopback link", {"Drv.DrvEvent", ""}},
  )

  // Document the subcomponents that this component has
  SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
      {"memory", "Interface to memory hierarchy", "Drv::DrvMemory"},
  )

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
  
  static constexpr uint32_t DEBUG_INIT  = (1<< 0); //!< debug messages during initialization
  static constexpr uint32_t DEBUG_CLK   = (1<<31); //!< debug messages we expect to see during clock ticks
  static constexpr uint32_t DEBUG_REQ   = (1<<30); //!< debug messages we expect to see when receiving requests
  static constexpr uint32_t DEBUG_RSP   = (1<<29); //!< debug messages we expect to see when receiving responses


  /**
   * initialize the component
   */
  void init(unsigned int phase) override;

  /**
   * setup the component
   */
  void setup() override;

  /**
   * finish the component
   */
  void finish() override;

  /**
   * return the id of a thread
   */
  int getThreadID(DrvThread *thread) {
    int tid = thread - &threads_[0];
    assert(tid >= 0 && tid < threads_.size());
    return tid;    
  }

  /**
   * return pointer to thread
   */
  DrvThread* getThread(int tid) {
    assert(tid >= 0 && tid < threads_.size());
    return &threads_[tid];
  }

  /**
   * return the time converter for the clock
   */
  SST::TimeConverter* getClockTC() {
    return clocktc_;
  }
  
private:  
  std::unique_ptr<SST::Output> output_; //!< for logging
  std::vector<DrvThread> threads_; //!< the threads on this core
  void *executable_; //!< the executable handle
  drv_api_main_t main_; //!< the main function in the executable
  drv_api_get_thread_context_t get_thread_context_; //!< the get_thread_context function in the executable
  drv_api_set_thread_context_t set_thread_context_; //!< the set_thread_context function in the executable
  DrvMemory* memory_;  //!< the memory hierarchy
  SST::TimeConverter *clocktc_; //!< the clock time converter
  int done_; //!< number of threads that are done
  int last_thread_; //!< last thread that was executed
  std::vector<char*> argv_; //!< the command line arguments
  
public:
  int id_; //!< the core id
};
}
}
