#pragma once
#include <DrvAPIAddress.hpp>
#include <DrvAPIThread.hpp>

namespace DrvAPI
{

/**
 * @brief the types of memory
 */
typedef enum __DrvAPIMemoryType {
    DrvAPIMemoryL1SP,
    DrvAPIMemoryDRAM,
    DrvAPIMemoryNTypes,
} DrvAPIMemoryType;

/**
 * @brief read from a memory address
 * 
 */
template <typename T>
T read(DrvAPIAddress address)
{
    T result = 0;
    DrvAPIThread::current()->setState(std::make_shared<DrvAPIMemReadConcrete<T>>(address));
    DrvAPIThread::current()->yield();
    auto read_req = std::dynamic_pointer_cast<DrvAPIMemRead>(DrvAPIThread::current()->getState());
    if (read_req) {
        read_req->getResult(&result);
    }
    return result;
}

/**
 * @brief write to a memory address
 * 
 */
template <typename T>
void write(DrvAPIAddress address, T value)
{
    DrvAPIThread::current()->setState(std::make_shared<DrvAPIMemWriteConcrete<T>>(address, value));
    DrvAPIThread::current()->yield();
}

/**
 * @brief atomic swap to a memory address
 * 
 */
template <typename T>
T atomic_swap(DrvAPIAddress address, T value)
{
    T result = 0;
    DrvAPIThread::current()->setState(std::make_shared<DrvAPIMemAtomicConcrete<T, DrvAPIMemAtomicSWAP>>(address, value));
    DrvAPIThread::current()->yield();
    auto atomic_req = std::dynamic_pointer_cast<DrvAPIMemAtomic>(DrvAPIThread::current()->getState());
    if (atomic_req) {
        atomic_req->getResult(&result);
    }
    return result;
}

/**
 * @brief atomic add to a memory address
 */
template <typename T>
T atomic_add(DrvAPIAddress address, T value)
{
    T result = 0;
    DrvAPIThread::current()->setState(std::make_shared<DrvAPIMemAtomicConcrete<T, DrvAPIMemAtomicADD>>(address, value));
    DrvAPIThread::current()->yield();
    auto atomic_req = std::dynamic_pointer_cast<DrvAPIMemAtomic>(DrvAPIThread::current()->getState());
    if (atomic_req) {
        atomic_req->getResult(&result);
    }
    return result;
}
}
