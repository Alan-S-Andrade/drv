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
  std::size_t data_size;
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
  DrvAPIFunctionFactory getFactory() {
    return getTypeInfo()->factory;
  }

  /**
   * @brief get the type id for this type
   */
  virtual DrvAPIFunctionTypeId getFunctionTypeId() = 0;

  /**
   * @brief get the type info for this function type
   *
   * This needs to exist and be a virtual function because it forces
   * the compiler to allocate the global type info variables
   * in the template subclasses if they are used to implement
   * this function (which they are :-D).
   *
   * Without a function like this, the compiler can optimize away static
   * constants like the type info structure.
   *
   * We don't want that because we rely on those constants to exist and
   * be allocated in our special defined drv_api_function_typev section.
   * This is other ranks can rematerialize the function from a type id.
   */
  virtual const DrvAPIFunctionTypeInfo *getTypeInfo() = 0;
  
  /**
   * @brief get the number of DrvAPIFunction types
   */
  static DrvAPIFunctionTypeId NumTypes() {
    return &__stop_drv_api_function_typev - &__start_drv_api_function_typev;
  }  

  /**
   * @brief get the factory for a given type id
   */  
  static DrvAPIFunctionFactory GetFactory(DrvAPIFunctionTypeId id) {
    return (&__start_drv_api_function_typev)[id].factory;
  }

  /**
   * @brief create a function from the type id and buffer
   */
  static DrvAPIFunction *FromIDAndBuffer(DrvAPIFunctionTypeId id, void *buf) {
    return GetFactory(id)(buf);
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

  /**
   * @brief GetTypeInfo
   *
   * This function exists as a shell for allocating a type info structure
   * for this function type.
   */
  static const DrvAPIFunctionTypeInfo &GetTypeInfo() {
    // we allocate this in the drv_api_function_typev section
    // so that other ranks can find it with an id
    __attribute__((section("drv_api_function_typev")))    
    static DrvAPIFunctionTypeInfo TYPE_INFO = {0, sizeof(F), Factory};
    return TYPE_INFO;
  }

  /**
   * @brief getTypeInfo
   *
   * implements the pure virtual function from DrvAPIFunction
   */
  const DrvAPIFunctionTypeInfo *getTypeInfo() override {
    return &GetTypeInfo();
  }

  /**
   * @brief getFunctionTypeId
   *
   * implements the pure virtual function from DrvAPIFunction
   */
  DrvAPIFunctionTypeId getFunctionTypeId() override {
    // use the address of the type info structure to get the id
    // by offsetting it from the start of the special section
    return (&GetTypeInfo()) - &__start_drv_api_function_typev;
  }
    
public:
  F f_;
};

/**
 * @brief Create a DrvAPIFunction from a functor
 */
template <typename F>
DrvAPIFunction* MakeDrvAPIFunction(const F & f)
{
  return new DrvAPIFunctionConcrete<F>(f);

}

}
