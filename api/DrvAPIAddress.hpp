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
   * @brief offset
   */
  uint64_t offset() const { return address_; }
  
private:
    uint64_t address_;
};

}
