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
  virtual void getResult(void *p) {}
  virtual void setResult(void *p) {}

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
  virtual void getPayload(void *p) {}
  virtual void setPayload(void *p) {}

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
private:
  T value_;
};

typedef enum  {
    DrvAPIMemAtomicSWAP,
    DrvAPIMemAtomicADD,
} DrvAPIMemAtomicType;

/**
 * @brief Base thread state for an atomic read-modify-write
 */
class DrvAPIMemAtomic : public DrvAPIMem {
public:
    DrvAPIMemAtomic(DrvAPIAddress address)
        : DrvAPIMem(), address_(address) {}
    DrvAPIAddress getAddress() const { return address_; }
    virtual void getPayload(void *p) {}
    virtual void setPayload(void *p) {}
    virtual void getResult(void *p) {}
    virtual void setResult(void *p) {}
    virtual void modify() {}
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
    virtual void getPayload(void *p) override {
        *static_cast<T*>(p) = w_value_;
    }
    virtual void setPayload(void *p) override {
        w_value_ = *static_cast<T*>(p);
    }
    virtual void getResult(void *p) override {
        *static_cast<T*>(p) = r_value_;
    }
    virtual void setResult(void *p) override {
        r_value_  = *static_cast<T*>(p);
    }
    virtual void modify() override {
        switch (OP) {
        case DrvAPIMemAtomicSWAP:
            break;
        case DrvAPIMemAtomicADD:
            w_value_ += r_value_;
            break;
        }
    }
private:
    T r_value_;
    T w_value_;
};
}
