#include "DrvCore.hpp"
#include <cstdio>

static constexpr uint32_t DEBUG_INIT  = (1<< 0); //!< debug messages during initialization
static constexpr uint32_t DEBUG_CLK   = (1<<31); //!< debug messages we expect to see during clock ticks
static constexpr uint32_t DEBUG_REQ   = (1<<30); //!< debug messages we expect to see when receiving requests
static constexpr uint32_t DEBUG_RSP   = (1<<29); //!< debug messages we expect to see when receiving responses
using namespace SST;
using namespace Drv;

//////////////////////////
// init and finish code //
//////////////////////////
/**
 * configure the output logging
 */
void DrvCore::configureOutput(SST::Params &params) {
  int verbose_level = params.find<int>("verbose");
  uint32_t verbose_mask = 0;
  if (params.find<bool>("debug_init"))
    verbose_mask |= DEBUG_INIT;
  if (params.find<bool>("debug_clock"))
    verbose_mask |= DEBUG_CLK;
  if (params.find<bool>("debug_requests"))
    verbose_mask |= DEBUG_REQ;
  if (params.find<bool>("debug_responses"))
    verbose_mask |= DEBUG_RSP;
    
  output_ = std::make_unique<SST::Output>("[DrvCore @t: @f:@l: @p] ", verbose_level, verbose_mask, Output::STDOUT);
  output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configured output logging\n");
}

/**
 * configure the clock and register the handler
 */
void DrvCore::configureClock(SST::Params &params) {
  registerClock(params.find<std::string>("clock", "125MHz"),
                new Clock::Handler<DrvCore>(this, &DrvCore::clockTick));    
}

/**
 * configure one thread
 */
void DrvCore::configureThread(int thread, int threads) {
  output_->verbose(CALL_INFO, 2, DEBUG_INIT, "configuring thread (%2d/%2d)\n", thread, threads);
  threads_.push_back(DrvThread());
}

/**
 * configure threads on the core
 */
void DrvCore::configureThreads(SST::Params &params) {
  int threads = params.find<int>("threads", 1);
  output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configuring %d threads\n", threads);
  for (int thread = 0; thread < threads; thread++)
    configureThread(thread, threads);
}

DrvCore::DrvCore(SST::ComponentId_t id, SST::Params& params)
  : SST::Component(id) {
  configureOutput(params);
  configureClock(params);
  configureThreads(params);
}

DrvCore::~DrvCore() {
}

/////////////////////
// clock tick code //
/////////////////////
static constexpr int NO_THREAD_READY = -1;

int DrvCore::selectReadyThread() {
    // select a ready thread to execute
    int thread_id = NO_THREAD_READY;
    // for (int i = 0; i < threads_.size(); i++) {
    //     if (threads_[i].isReady()) {
    //         thread_id = i;
    //         break;
    //     }
    // }
    return thread_id;
}


void DrvCore::executeReadyThread() {
  // select a ready thread to execute
  int thread_id = selectReadyThread();
  if (thread_id == NO_THREAD_READY) {
    return;
  }

  // execute the ready thread
  threads_[thread_id].execute(this);
}

bool DrvCore::allDone() {
  return true;
}

bool DrvCore::clockTick(SST::Cycle_t cycle) {
  output_->verbose(CALL_INFO, 1, DEBUG_CLK, "tick!\n");
  // execute a ready thread
  executeReadyThread();
  return allDone();
}
