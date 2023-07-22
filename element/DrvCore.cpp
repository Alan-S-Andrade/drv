#include "DrvCore.hpp"
#include "DrvSimpleMemory.hpp"
#include "DrvSelfLinkMemory.hpp"
#include "DrvStdMemory.hpp"
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
  clocktc_ = registerClock(params.find<std::string>("clock", "125MHz"),
                new Clock::Handler<DrvCore>(this, &DrvCore::clockTick));    
}

/**
 * configure one thread
 */
void DrvCore::configureThread(int thread, int threads) {
  output_->verbose(CALL_INFO, 2, DEBUG_INIT, "configuring thread (%2d/%2d)\n", thread, threads);
  threads_.emplace_back();
  threads_.back().getAPIThread().setMain(main_);
  threads_.back().getAPIThread().setId(thread);
  threads_.back().getAPIThread().setCoreId(id_);
}

/**
 * configure threads on the core
 */
void DrvCore::configureThreads(SST::Params &params) {
  int threads = params.find<int>("threads", 1);
  output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configuring %d threads\n", threads);
  for (int thread = 0; thread < threads; thread++)
    configureThread(thread, threads);
  done_ = threads;
  last_thread_ = threads - 1;
}

/**
 * configure the core link
 */
SST::Link* DrvCore::configureCoreLink(const std::string &link_name, Event::HandlerBase *handler) {
    return configureLink(link_name, handler);
}

/**
 * configure standard memory subcomponent
 */
SST::Interfaces::StandardMem *
DrvCore::loadStandardMemSubComponent(const std::string &mem_name, uint64_t share_flags, SST::Interfaces::StandardMem::HandlerBase *handler) {
    return loadUserSubComponent<SST::Interfaces::StandardMem>(mem_name, share_flags, clocktc_, handler);
}
    
/**
 * configure the memory
 */
void DrvCore::configureMemory(SST::Params &params) {
    if (isPortConnected("mem_loopback")) {
        output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configuring memory loopback\n");
        memory_ = new DrvSelfLinkMemory(this, "mem_loopback");
    } else if (isUserSubComponentLoadableUsingAPI<Interfaces::StandardMem>("memory")) {
        output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configuring standard memory\n");
        memory_ = new DrvStdMemory(this, "memory");
    } else {
        output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configuring simple memory\n");
        memory_ = new DrvSimpleMemory();
    }
}

DrvCore::DrvCore(SST::ComponentId_t id, SST::Params& params)
  : SST::Component(id)
  , executable_(nullptr) {
  id_ = params.find<int>("id", 0);
  configureOutput(params);
  configureClock(params);
  configureExecutable(params);
  configureThreads(params);
  configureMemory(params);
}

DrvCore::~DrvCore() {
    threads_.clear();
    // the last thing we should is close the executable
    // this keeps the vtable entries valid for dynamic classes
    // created in the user code
    closeExecutable();
    delete memory_;
}

///////////////////////////
// SST simulation phases //
///////////////////////////
/**
 * initialize the component
 */
void DrvCore::init(unsigned int phase) {
  auto stdmem = dynamic_cast<DrvStdMemory*>(memory_);
  if (stdmem) {
    stdmem->init(phase);
  }
}

/**
 * finish the component
 */
void DrvCore::setup() {
  auto stdmem = dynamic_cast<DrvStdMemory*>(memory_);
  if (stdmem) {
    stdmem->setup();
  }  
}

/**
 * finish the component
 */
void DrvCore::finish() {
  auto stdmem = dynamic_cast<DrvStdMemory*>(memory_);
  if (stdmem) {
    stdmem->finish();
  }
}

/////////////////////
// clock tick code //
/////////////////////
static constexpr int NO_THREAD_READY = -1;

int DrvCore::selectReadyThread() {
  // select a ready thread to execute
  for (int t = 0; t < threads_.size(); t++) {
    int thread_id = (last_thread_ + t + 1) % threads_.size();
    DrvThread *thread = getThread(thread_id);
    auto state = thread->getAPIThread().getState();
    if (state->canResume()) {
      output_->verbose(CALL_INFO, 2, DEBUG_CLK, "thread %d is ready\n", thread_id);
      return thread_id;
    }
  }
  output_->verbose(CALL_INFO, 2, DEBUG_CLK, "no thread is ready\n");
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
  last_thread_ = thread_id;
}

void DrvCore::handleThreadStateAfterYield(DrvThread *thread) {
  std::shared_ptr<DrvAPI::DrvAPIThreadState> state = thread->getAPIThread().getState();
  std::shared_ptr<DrvAPI::DrvAPIMem> mem_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMem>(state);
  // handle memory requests
  if (mem_req) {
    // for now; do nothing
    memory_->sendRequest(this, thread, mem_req);
    return;
  }
  // handle termination
  std::shared_ptr<DrvAPI::DrvAPITerminate> term_req = std::dynamic_pointer_cast<DrvAPI::DrvAPITerminate>(state);
  if (term_req) {
    output_->verbose(CALL_INFO, 1, DEBUG_CLK, "thread %d terminated\n", getThreadID(thread));
    done_--;
  }
  return;
}
    
bool DrvCore::allDone() {
  return done_ == 0;
}

bool DrvCore::clockTick(SST::Cycle_t cycle) {
  output_->verbose(CALL_INFO, 1, DEBUG_CLK, "tick!\n");
  // execute a ready thread
  executeReadyThread();
  return allDone();
}

