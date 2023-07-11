#pragma once
#include <DrvAPIAddress.hpp>
namespace DrvAPI
{
/**
 * @brief The thread state
 * 
 */
class DrvAPIThreadState
{
public:
  DrvAPIThreadState() {}
  virtual ~DrvAPIThreadState() {}
};

/**
 * @brief The thread state for an idle thread
 * 
 */
class DrvAPIThreadIdle : public DrvAPIThreadState
{
public:
  DrvAPIThreadIdle() {}
  ~DrvAPIThreadIdle() {}
};

/**
 * @brief Base thread state for a memory read
 * 
 * @tparam T 
 */
class DrvAPIMemRead : public DrvAPIThreadState
{
public:
  DrvAPIMemRead(DrvAPIAddress address)
    : address_(address) {}
  ~DrvAPIMemRead() {}
  DrvAPIAddress getAddress() const { return address_; }
  virtual void getResult(void *p) = 0;
  virtual void setResult(void *p) = 0;
private:
  DrvAPIAddress address_;
};

/**
 * @brief Concrete thread state for a memory read
 * 
 * @tparam T 
 */
template <typename T>
class DrvAPIMemReadConcrete : public DrvAPIMemRead
{
public:
  DrvAPIMemReadConcrete(DrvAPIAddress address, T value)
      : DrvAPIMemRead(address), value_(value) {}
  ~DrvAPIMemReadConcrete() {}

  void getResult(void *p) override {
    *static_cast<T*>(p) = value_;
  }

  void setResult(void *p) override {
    value_ = *static_cast<T*>(p);
  }
  
private:
  T value_;
};

/**
 * @brief Base thread state for a memory write
 * 
 * @tparam T 
 */
class DrvAPIMemWrite : public DrvAPIThreadState
{
public:
  DrvAPIMemWrite(DrvAPIAddress address)
    : address_(address) {}
  ~DrvAPIMemWrite() {}
  DrvAPIAddress getAddress() const { return address_; }
  virtual void getResult(void *p) = 0;
  virtual void setResult(void *p) = 0;
private:
  DrvAPIAddress address_;
};

/**
 * @brief Concrete thread state for a memory write
 * 
 * @tparam T 
 */
template <typename T>
class DrvAPIMemWriteConcrete : public DrvAPIMemWrite
{
public:
  DrvAPIMemWriteConcrete(DrvAPIAddress address, T value)
      : DrvAPIMemWrite(address), value_(value) {}
  ~DrvAPIMemWriteConcrete() {}

  void getResult(void *p) override {
    *static_cast<T*>(p) = value_;
  }

  void setResult(void *p) override {
    value_ = *static_cast<T*>(p);
  }
private:
  T value_;
};

}
