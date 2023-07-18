#include <DrvThread.hpp>
#include <DrvCore.hpp>

using namespace SST;
using namespace Drv;

/**
 * Constructor
 */
DrvThread::DrvThread() {
    thread_ = new DrvAPI::DrvAPIThread();
}

/**
 * Destructor
 */
DrvThread::~DrvThread() {
    delete thread_;
}

/**
 * Execute the thread
 */
void DrvThread::execute(DrvCore *core) {
    core->setThreadContext(this);
    //core->output()->verbose(CALL_INFO, 2, DrvCore::DEBUG_CLK, "Thread %p: can resume = %d\n", this, static_cast<int>(this->getAPIThread().getState()->canResume()));
    thread_->resume();
    core->handleThreadStateAfterYield(this);
}
