#pragma once
#include <DrvAPIThreadState.hpp>
#include <DrvAPIMain.hpp>
#include <boost/coroutine2/all.hpp>
#include <memory>
namespace DrvAPI
{
class DrvAPIThread
{
public:
  using coro_t = boost::coroutines2::coroutine<void>;

  /**
   * @brief Construct a new DrvAPIThread object
   */
  DrvAPIThread();

  /**
   * @brief Destroy the DrvAPIThread object
   */
  ~DrvAPIThread(){}

  /**
   * @brief Yield back to the main context
   *
   * @param state
   */
  void yield(DrvAPIThreadState *state);

  /**
   * @brief Yield back to the main context
   */
  void yield();
  
  /**
   * @brief Resume the thread context
   */
  void resume();

  /**
   * @brief Set the state object
   * 
   * @param state 
   */
  void setState(const std::shared_ptr<DrvAPIThreadState> &state) { state_ = state; }

  /**
   * @brief Get the state object
   * 
   * @return std::shared_ptr<DrvAPIThreadState> 
   */
  std::shared_ptr<DrvAPIThreadState> getState() const { return state_; }

  /**
   * @brief Set the main function
   *
   * @param main
   */
  void setMain(drv_api_main_t main) { main_ = main; }
  
  /**
   * @brief Get the current active thread
   * 
   * @return DrvAPIThread* 
   */
  static DrvAPIThread* current() { return g_current_thread; } //!< Get the current active thread

  thread_local static DrvAPIThread *g_current_thread; //!< The current active thread  

private:
  std::unique_ptr<coro_t::pull_type> thread_context_; //!< Thread context, coroutine that can be resumed
  coro_t::push_type *main_context_; //!< Main context, can be yielded back to
  std::shared_ptr<DrvAPIThreadState> state_; //!< Thread state
  drv_api_main_t main_; //!< Main function
};
}

/**
 * @brief Get the current active thread
 * 
 * @return DrvAPIThread* 
 */
extern "C" DrvAPI::DrvAPIThread *DrvAPIGetCurrentContext();

/**
 * type definition for DrvAPIGetCurrentContext
 */
typedef DrvAPI::DrvAPIThread (*drv_api_get_thread_context_t)();

/**
 * @brief Set the current active thread
 * 
 * @param thread 
 */
extern "C" void DrvAPISetCurrentContext(DrvAPI::DrvAPIThread *thread);

/**
 * type definition for DrvAPISetCurrentContext
 */
typedef void (*drv_api_set_thread_context_t)(DrvAPI::DrvAPIThread *thread);
