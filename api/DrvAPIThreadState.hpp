// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#ifndef DRV_API_THREAD_STATE_H
#define DRV_API_THREAD_STATE_H
#include <DrvAPIAddress.hpp>
#include <DrvAPIReadModifyWrite.hpp>
#include <stdlib.h>
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
  virtual bool canResume() const { return true; }
};

/**
 * @brief The thread state for an idle thread
 * 
 */
class DrvAPIThreadIdle : public DrvAPIThreadState
{
public:
  DrvAPIThreadIdle() {}
};

/**
 * @brief Terminated thread state
 */
class DrvAPITerminate : public DrvAPIThreadState
{
public:
  DrvAPITerminate() {}
  bool canResume() const override { return false; }
};

/**
 */
class DrvAPINop : public DrvAPIThreadState
{
public:
    DrvAPINop(int count) : can_resume_(false), count_(count) {}
    bool canResume() const override { return can_resume_; }
    int count() const { return count_; }
    void complete(){ can_resume_ = true; }
private:
    bool can_resume_;
    int  count_;
};

/**
 * @brief Base thread state for a memory operation
 * 
 */
class DrvAPIMem : public DrvAPIThreadState
{
public:
  DrvAPIMem() : can_resume_(false) {}

  virtual bool canResume() const  { return can_resume_; }
  
  void complete() { can_resume_ = true; }
protected:
  bool can_resume_;
};

/**
 * @brief Base thread state for a memory read
 * 
 * @tparam T 
 */
class DrvAPIMemRead : public DrvAPIMem
{
public:
  DrvAPIMemRead(DrvAPIAddress address)
    : DrvAPIMem(), address_(address) {}

  DrvAPIAddress getAddress() const { return address_; }
  virtual void getResult(void *p) = 0;
  virtual void setResult(void *p) = 0;
  virtual size_t getSize() const { return 0; }
protected:
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
  DrvAPIMemReadConcrete(DrvAPIAddress address)
      : DrvAPIMemRead(address) {}

  virtual void getResult(void *p) override {
    *static_cast<T*>(p) = value_;
  }

  virtual void setResult(void *p) override {
    value_ = *static_cast<T*>(p);
  }

  virtual size_t getSize() const override {
    return sizeof(T);
  }
  
private:
  T value_;
};

/**
 * @brief Base thread state for a memory write
 * 
 * @tparam T 
 */
class DrvAPIMemWrite : public DrvAPIMem
{
public:
  DrvAPIMemWrite(DrvAPIAddress address)
    : DrvAPIMem(), address_(address) {}
  DrvAPIAddress getAddress() const { return address_; }
  virtual void getPayload(void *p) = 0;
  virtual void setPayload(void *p)  = 0;
  virtual size_t getSize() const { return 0; }

protected:
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
  virtual void getPayload(void *p) override {
    *static_cast<T*>(p) = value_;
  }

  virtual void setPayload(void *p) override {
    value_ = *static_cast<T*>(p);
  }

  virtual size_t getSize() const override {
    return sizeof(T);
  }
  
private:
  T value_;
};


/**
 * @brief Base thread state for an atomic read-modify-write
 */
class DrvAPIMemAtomic : public DrvAPIMem {
public:
  DrvAPIMemAtomic(DrvAPIAddress address)
    : DrvAPIMem(), address_(address) {}
  DrvAPIAddress getAddress() const { return address_; }
  virtual void getPayload(void *p) = 0;
  virtual void setPayload(void *p)  = 0;
  virtual void getResult(void *p) = 0;
  virtual void setResult(void *p) = 0;
  virtual void modify() = 0;
  virtual size_t getSize() const { return 0; }
  virtual DrvAPIMemAtomicType getOp() const = 0;
protected:    
  DrvAPIAddress address_;
};

/**
 * @brief Concrete thread state for an atomic read-modify-write
 * 
 * @tparam T The data type.
 * @tparam op The operation.
 */
template <typename T, DrvAPIMemAtomicType OP>
class DrvAPIMemAtomicConcrete : public DrvAPIMemAtomic {
public:
  DrvAPIMemAtomicConcrete(DrvAPIAddress address, T value)
    : DrvAPIMemAtomic(address), w_value_(value) {}
  void getPayload(void *p) override {
    *static_cast<T*>(p) = w_value_;
  }
  void setPayload(void *p) override {
    w_value_ = *static_cast<T*>(p);
  }
  void getResult(void *p) override {
    *static_cast<T*>(p) = r_value_;
  }
  void setResult(void *p) override {
    r_value_  = *static_cast<T*>(p);
  }
  void modify() override {
    w_value_ = atomic_modify<T>(w_value_, r_value_, OP);
  }
  size_t getSize() const override { return sizeof(T); }
  DrvAPIMemAtomicType getOp() const override { return OP; }
private:
  T r_value_;
  T w_value_;
};
}
#endif
