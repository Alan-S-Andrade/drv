#pragma once
#include <sst/core/component.h>
#include <sst/core/link.h>
#include <memory>
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
                          /* system config */
                          {"threads", "Number of threads on this core", "1"},
                          {"clock", "Clock rate of core", "125MHz"},
                          /* debug flags */
                          {"verbose", "Verbosity of logging", "0"},
                          {"debug_init", "Print debug messages during initialization", "False"},
                          {"debug_clock", "Print debug messages we expect to see during clock ticks", "False"},
                          {"debug_requests", "Print debug messages we expect to see during request events"},
                          {"debug_responses", "Print debug messages we expect to see during response events"},
                          )
  // Document the ports that this component accepts
#if 0
  SST_ELI_DOCUMENT_PORTS(
                         ("placeholder", "A link", {"pandos.PandosPacketEvent", ""}),
                         )
#endif

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

private:  
  std::unique_ptr<SST::Output> output_; //!< for logging
  std::vector<DrvThread> threads_; //!< the threads on this core
  void *executable_; //!< the executable handle
  drv_api_main_t main_; //!< the main function in the executable
  drv_api_get_thread_context_t get_thread_context_; //!< the get_thread_context function in the executable
  drv_api_set_thread_context_t set_thread_context_; //!< the set_thread_context function in the executable
  int count_down_;

  // memory
  std::unique_ptr<DrvMemory> memory_;  
};
}
}
