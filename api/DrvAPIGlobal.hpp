// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#ifndef DRV_API_GLOBAL_H
#define DRV_API_GLOBAL_H
#include <DrvAPISection.hpp>
#include <DrvAPIPointer.hpp>
#include <DrvAPIInfo.hpp>
#include <atomic>
#include <cassert>
namespace DrvAPI
{

/**
 * statically allocatable data; can be allocated in special memory regions
 */
template <typename T, DrvAPI::DrvAPIMemoryType MEMTYPE>
class static_data : public value_handle<T> {
public:
    /**
     * @brief constructor
     */
    static_data() {
        _offset = DrvAPI::DrvAPISection::GetSection(MEMTYPE).increaseSizeBy(sizeof(T));
    }
    static_data(const static_data &other) = delete;
    static_data(static_data &&other) = delete;
    ~static_data() = default;

    /**
     * handle assignment is a deep copy
     */
    static_data & operator=(const static_data &other) {
        value_handle<T> me(address());
        value_handle<T> you(other.address());
        me = you;
        return *this;
    }

    /**
     * handle assignment is a deep copy
     */    
    static_data & operator=(static_data &&other) {
        value_handle<T> me(address());
        value_handle<T> you(other.address());
        me = you;
        return *this;
    }

    /**
     * assignment operators
     */
    static_data & operator=(const T &v) {
        value_handle<T> handle(address());
        handle = v;
        return *this;
    }

    /**
     * materialize the address of the static data
     */
    DrvAPI::DrvAPIAddress address() const override {
        DrvAPI::DrvAPIAddress r = DrvAPI::DrvAPISection::GetSection(MEMTYPE)
            .getBase(DrvAPI::myPXNId(), DrvAPI::myPodId(), DrvAPI::myCoreId())
            + _offset;
        return r;   
    }
    

    DrvAPI::DrvAPIAddress _offset;
};

/**
 * static data in L1SP
 */
template <typename T>
using l1sp_static = static_data<T, DrvAPI::DrvAPIMemoryType::DrvAPIMemoryL1SP>;

/**
 * static data in L2SP
 */
template <typename T>
using l2sp_static = static_data<T, DrvAPI::DrvAPIMemoryType::DrvAPIMemoryL2SP>;

/**
 * static data in DRAM
 */
template <typename T>
using dram_static = static_data<T, DrvAPI::DrvAPIMemoryType::DrvAPIMemoryDRAM>;
  
template <typename T, DrvAPIMemoryType MEMTYPE>
using DrvAPIGlobal = static_data<T, MEMTYPE>;

template <typename T>
using DrvAPIGlobalL1SP = DrvAPIGlobal<T, DrvAPIMemoryType::DrvAPIMemoryL1SP>;

template <typename T>
using DrvAPIGlobalL2SP = DrvAPIGlobal<T, DrvAPIMemoryType::DrvAPIMemoryL2SP>;

template <typename T>
using DrvAPIGlobalDRAM = DrvAPIGlobal<T, DrvAPIMemoryType::DrvAPIMemoryDRAM>;
}
#endif
