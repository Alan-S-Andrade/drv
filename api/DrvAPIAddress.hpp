// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#ifndef DRV_API_ADDRESS_H
#define DRV_API_ADDRESS_H
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


  /**
   * postfix increment
   */
  DrvAPIAddress operator++(int) {
    DrvAPIAddress tmp(*this);
    address_++;
    return tmp;
  }

  /**
   * prefix increment
   */
  DrvAPIAddress &operator++() {
    address_++;
    return *this;
  }

  /**
   * addition assignment operator
   */
  DrvAPIAddress &operator+=(uint64_t rhs) {
    address_ += rhs;
    return *this;
  }

  /**
   * subtraction assignment operator
   */
  DrvAPIAddress &operator-=(uint64_t rhs) {
    address_ -= rhs;
    return *this;
  }
  
  DrvAPIAddress operator+=(const DrvAPIAddress &rhs) {
    address_ += rhs.address_;
    return *this;
  }

private:
  uint64_t address_;
};

}
#endif
