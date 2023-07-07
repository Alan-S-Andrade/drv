#pragma once
namespace SST {
namespace Fwd {

// forward declarations
class FwdCore;

/**
 * Forward thread
 */
class FwdThread {
public:
  /**
    * Constructor
    */
  FwdThread();

  /**
    * Destructor
    */
  ~FwdThread();

  /**
    * Execute the thread
    */
  void execute(FwdCore *core);
};

}
}
