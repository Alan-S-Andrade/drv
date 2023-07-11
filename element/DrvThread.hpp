#pragma once
#include "DrvAPIThread.hpp"
#include <memory>
namespace SST {
namespace Drv {

// forward declarations
class DrvCore;

/**
 * Forward thread
 */
class DrvThread {
public:
  /**
    * Constructor
    */
    DrvThread();

  /**
    * Destructor
    */
  ~DrvThread();

  /**
    * Execute the thread
    */
  void execute(DrvCore *core);

  /**
    * Get the api thread
    */
  DrvAPI::DrvAPIThread &getAPIThread() { return *thread_; }

  /**
    * Get the api thread
    */
  const DrvAPI::DrvAPIThread &getAPIThread() const { return *thread_; }
    
private:
  DrvAPI::DrvAPIThread* thread_;
};

}
}
