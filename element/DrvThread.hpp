#pragma once
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
};

}
}
