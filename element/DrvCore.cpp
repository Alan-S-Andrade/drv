#include "DrvCore.hpp"
#include <cstdio>
#include <dlfcn.h>

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
 * configure the executable
 */
void DrvCore::configureExecutable(SST::Params &params) {
  std::string executable = params.find<std::string>("executable");
  if (executable.empty()) {
    output_->fatal(CALL_INFO, -1, "executable not specified\n");
  }

  output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configuring executable: %s\n", executable.c_str());
  executable_ = dlopen(executable.c_str(), RTLD_LAZY|RTLD_LOCAL);
  if (!executable_) {
    output_->fatal(CALL_INFO, -1, "unable to open executable: %s\n", dlerror());
  }

  main_ = (drv_api_main_t)dlsym(executable_, "__drv_api_main");
  if (!main_) {
    output_->fatal(CALL_INFO, -1, "unable to find __drv_api_main in executable: %s\n", dlerror());
  }

  get_thread_context_ = (drv_api_get_thread_context_t)dlsym(executable_, "DrvAPIGetCurrentContext");
  if (!get_thread_context_) {
    output_->fatal(CALL_INFO, -1, "unable to find DrvAPIGetCurrentContext in executable: %s\n", dlerror());
  }

  set_thread_context_ = (drv_api_set_thread_context_t)dlsym(executable_, "DrvAPISetCurrentContext");
  if (!set_thread_context_) {
    output_->fatal(CALL_INFO, -1, "unable to find DrvAPISetCurrentContext in executable: %s\n", dlerror());
  }

  output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configured executable\n");  
}

/**
 * close the executable
 */
void DrvCore::closeExecutable() {
  if (dlclose(executable_)) {
    output_->fatal(CALL_INFO, -1, "unable to close executable: %s\n", dlerror());
  }
  executable_ = nullptr;
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
  threads_.emplace_back();
  threads_.back().getAPIThread().setMain(main_);
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
  : SST::Component(id)
  , executable_(nullptr)
  , count_down_(32){
  configureOutput(params);
  configureClock(params);
  configureExecutable(params);
  configureThreads(params);
}

DrvCore::~DrvCore() {
  closeExecutable();
}

/////////////////////
// clock tick code //
/////////////////////
static constexpr int NO_THREAD_READY = -1;

int DrvCore::selectReadyThread() {
  // select a ready thread to execute
  for (int t = 0; t < threads_.size(); t++) {
    DrvThread &thread = threads_[t];
    auto state = thread.getAPIThread().getState();
    if (state->canResume()) {
      return t;
    }
  }
  return NO_THREAD_READY;
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
  return --count_down_ == 0;
}

bool DrvCore::clockTick(SST::Cycle_t cycle) {
  output_->verbose(CALL_INFO, 1, DEBUG_CLK, "tick!\n");
  // execute a ready thread
  executeReadyThread();
  return allDone();
}
