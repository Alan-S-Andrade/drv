#pragma once
#include <cstdint>
namespace DrvAPI
{
/**
 * @brief The address class
 * 
 */
class DrvAPIAddress
{
public:
  /**
   * @brief Construct a new DrvAPIAddress object
   * 
   * @param address 
   */
  DrvAPIAddress(uint64_t address)
    : address_(address) {}

  /**
   * no arg constructor
   */
  DrvAPIAddress() : address_(0) {}

  /**
   * @brief offset
   */
  uint64_t offset() const { return address_; }

  /**
   * cast operator
   */
  operator uint64_t() const { return address_; }


  DrvAPIAddress operator+=(const DrvAPIAddress &rhs) {
    address_ += rhs.address_;
    return *this;
  }

private:
  uint64_t address_;
};

}
