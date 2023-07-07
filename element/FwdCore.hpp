#pragma once
#include <sst/core/component.h>
#include <sst/core/link.h>
#include <memory>
#include <FwdThread.hpp>

namespace SST {
namespace Fwd {
/**
 * A Core
 */
class FwdCore : public SST::Component {
public:
  // REGISTER THIS COMPONENT INTO THE ELEMENT LIBRARY
  SST_ELI_REGISTER_COMPONENT(
                             FwdCore,
                             "Fwd",
                             "FwdCore",
                             SST_ELI_ELEMENT_VERSION(1,0,0),
                             "Fwd Core",
                             COMPONENT_CATEGORY_UNCATEGORIZED
                             )
  // Document the parameters that this component accepts
  SST_ELI_DOCUMENT_PARAMS(
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
  FwdCore(SST::ComponentId_t id, SST::Params& params);

  /** destructor */
  ~FwdCore();

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

private:
  std::unique_ptr<SST::Output> output_;
  std::vector<FwdThread> threads_;
};
}
}
