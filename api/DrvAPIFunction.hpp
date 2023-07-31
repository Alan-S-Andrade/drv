#pragma once
#include <cstdint>
#include <cstring>
#include <DrvAPIMemory.hpp>
namespace DrvAPI
{

// forward declaration
class DrvAPIFunction;

/**
 * @brief The type id of a DrvAPIFunction
 */
typedef int DrvAPIFunctionTypeId;

/**
 * Factory for building a DrvAPIFunction
 */
typedef DrvAPIFunction* (*DrvAPIFunctionFactory)(void *);

struct DrvAPIFunctionTypeInfo {
  int id;
  DrvAPIFunctionFactory factory;
};

// forward declaration; these symbols should be provided by the linker
extern "C" DrvAPIFunctionTypeInfo __start_drv_api_function_typev;
extern "C" DrvAPIFunctionTypeInfo  __stop_drv_api_function_typev;

/**
 * A functor that can be serialized and written to a DrvAPIAddress
 *
 * some rules: function pointers are not allowed, only functors
 */
class DrvAPIFunction
{
public:
  DrvAPIFunction() {}
  virtual ~DrvAPIFunction() {}

  /**
   * @brief execute
   * execute this function
   */
  virtual void execute() = 0;

  /**
   * @brief get a factory for this type
   */
  virtual DrvAPIFunctionFactory getFactory() = 0;  
  virtual DrvAPIFunctionTypeInfo *getTypeInfo() = 0;
  
  /**
   * @brief get the number of DrvAPIFunction types
   */
  static DrvAPIFunctionTypeId NumTypes() {
    return &__stop_drv_api_function_typev - &__start_drv_api_function_typev;
  }  

  /**
   * @brief get the function from the type id
   */
  static DrvAPIFunctionFactory GetFactory(DrvAPIFunctionTypeId id) {
    return (&__start_drv_api_function_typev)[id].factory;
  }
};


/**
 * @brief The DrvAPIFunctionConcrete class
 * @tparam F A functor type
 */
template <typename F>
class DrvAPIFunctionConcrete : public DrvAPIFunction
{
public:
  /**
   * @brief DrvAPIFunction
   * @param f
   * @note f must be a functor
   */
  DrvAPIFunctionConcrete(const F & f) : f_(f) {}

  /*
   * constructors/destructors/assignement operators
   */     
  ~DrvAPIFunctionConcrete()  = default;
  DrvAPIFunctionConcrete(const DrvAPIFunctionConcrete&) = default;
  DrvAPIFunctionConcrete& operator=(const DrvAPIFunctionConcrete&) = default;
  DrvAPIFunctionConcrete(DrvAPIFunctionConcrete&&) = default;
  DrvAPIFunctionConcrete& operator=(DrvAPIFunctionConcrete&&) = default;

  /**
   * Factory function for this class
   *
   * Converts a buffer to a DrvAPIFunctionConcrete<F>
   */
  static DrvAPIFunction* Factory(void *buf) {
    return new DrvAPIFunctionConcrete<F>(*(F*)buf);
  }
  
  /**
   * @brief execute
   * execute this function
   */
  void execute() override { f_(); }
  
  __attribute__((noinline))
  static DrvAPIFunctionTypeInfo &GetTypeInfo() {
    __attribute__((section("drv_api_function_typev")))    
    static DrvAPIFunctionTypeInfo TYPE_INFO = {0, Factory};
    return TYPE_INFO;
  }

  DrvAPIFunctionTypeInfo *getTypeInfo() override {
    return &GetTypeInfo();
  }
  
  /**
   * @brief class function get a factory for this type
   */
  static DrvAPIFunctionFactory GetFactory() {
    printf("TRACE: %s\n", __PRETTY_FUNCTION__);
    return GetTypeInfo().factory;
  }
  
  /**
   * @brief get a factory for this type
   */
  DrvAPIFunctionFactory getFactory() override {
    printf("TRACE: %s\n", __PRETTY_FUNCTION__);
    return GetFactory();
  }
    
public:
  F f_;
};

template <typename F>
DrvAPIFunction* MakeDrvAPIFunction(const F & f)
{
  printf("TRACE: %s\n", __PRETTY_FUNCTION__);
  return new DrvAPIFunctionConcrete<F>(f);

}

}
