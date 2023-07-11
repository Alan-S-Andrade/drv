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
    thread_->resume();
}
