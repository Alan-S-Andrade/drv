// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#ifndef DRV_API_ALLOCATOR_H
#define DRV_API_ALLOCATOR_H

#include <DrvAPIPointer.hpp>
#include <DrvAPIMemory.hpp>

namespace DrvAPI
{

/**
 * @brief Allocate memory
 * 
 * @param type 
 * @param size 
 * @return DrvAPIPointer<uint8_t> 
 */
DrvAPIPointer<void> DrvAPIMemoryAlloc(DrvAPIMemoryType type, size_t size);

/**
 * @brief Allocate memory
 * 
 * @param type 
 * @param size 
 * @return DrvAPIPointer<uint8_t> 
 */
void DrvAPIMemoryFree(const DrvAPIPointer<void> &ptr, size_t size);


/**
 * @brief Initialize the memory allocator
 * 
 */
void DrvAPIMemoryAllocatorInit();

/**
 * @brief Initialize the allocator for a memory type
 */
void DrvAPIMemoryAllocatorInitType(DrvAPIMemoryType type);

/**
 * @brief allocate specific type
 * @tparam the type
 * @param the memory type
 * return the pointer
 */
template <typename T>
inline DrvAPIPointer<T> DrvAPIMemoryAllocateType(DrvAPIMemoryType type)
{
    return (DrvAPIPointer<T>)DrvAPIMemoryAlloc(type, sizeof(T));
}

/**
 * @brief deallocate specific type
 * @tparam the type
 * @param the pointer
 */
template <typename T>
inline void DrvAPIMemoryDeallocateType(const DrvAPIPointer<T> &ptr)
{
    DrvAPIMemoryFree((DrvAPIPointer<void>)ptr, sizeof(T));
}

}
#endif
