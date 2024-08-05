// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#ifndef DRV_API_VAR_H
#define DRV_API_VAR_H
#include <DrvAPIPointer.hpp>
#include <DrvAPIInfo.hpp>
#include <DrvAPINativeToAddress.hpp>
#include <atomic>
#include <cassert>
#include <cstdlib>
namespace DrvAPI
{  

/**
 * dynamic data; can be allocated in special memory regions
 */
template <typename T, DrvAPI::DrvAPIMemoryType MEMTYPE>
class dynamic_data : public value_handle<T> {
public:
    /**
     * @brief constructor
     */
    dynamic_data():
        value_handle<T>(DrvAPI::DrvAPIMemoryAllocateType<T>(MEMTYPE)) {
    }

    ~dynamic_data() {
        DrvAPI::DrvAPIMemoryDeallocateType<T>(this->_ptr);
    }

    dynamic_data(const T&v) :
        dynamic_data() {
        value_handle<T> handle(this->_ptr);
        handle = v;
    }

    dynamic_data(const dynamic_data &other) = delete;
    dynamic_data(dynamic_data &&other) {
        this->_ptr = other._ptr;
        other._ptr = 0;
    }

    dynamic_data & operator=(const dynamic_data &other) {
        value_handle<T> me(this->_ptr);
        value_handle<T> you(other._ptr);
        me = you;
        return *this;
    }

    dynamic_data & operator=(dynamic_data &&other) = delete;

    dynamic_data & operator=(const T &v) {
        value_handle<T> handle(this->_ptr);
        handle = v;
        return *this;
    }
};

/**
 * dynamic data in L1SP
 */
template <typename T>
using l1sp_dynamic = dynamic_data<T, DrvAPI::DrvAPIMemoryType::DrvAPIMemoryL1SP>;

/**
 * dynamic data in L2SP
 */
template <typename T>
using l2sp_dynamic = dynamic_data<T, DrvAPI::DrvAPIMemoryType::DrvAPIMemoryL2SP>;

/**
 * dynamic data in L3SP
 */
template <typename T>
using dram_dynamic = dynamic_data<T, DrvAPI::DrvAPIMemoryType::DrvAPIMemoryDRAM>;

/**
 * alias DrvAPIVar<T>
 */
template <typename T>
using DrvAPIVar = l1sp_dynamic<T>;

}
#endif
