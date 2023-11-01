// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#ifndef DRV_API_GLOBAL_H
#define DRV_API_GLOBAL_H
#include <DrvAPIPointer.hpp>
#include <atomic>
#include <cassert>
namespace DrvAPI
{

class DrvAPISection
{
public:
    /**
     * @brief constructor
     */
    DrvAPISection() : base_(0), size_(0) {}

    /**
     * @brief get the section base
     */
    uint64_t getBase() const { return base_; }

    /**
     * @brief get the section size
     */
    uint64_t getSize() const { return size_; }

    /**
     * @brief set the section base
     */
    void setBase(uint64_t base) { base_ = base; }

    /**
     * @brief set the section size
     */
    void setSize(uint64_t size) { size_ = size; }

    /**
     * @brief increase the section size by specified bytes
     *
     * @return the section size previous to resizing
     */
    uint64_t increaseSizeBy(uint64_t incr_size) {
        // incr_size should be 8-byte aligned
        incr_size = (incr_size + 7) & ~7;
        assert(incr_size > 0 && "incr_size should be positive");
        return size_.fetch_add(incr_size);
    }

    /**
     * @brief get the section of the specified memory type
     */
    static DrvAPISection & GetSection(DrvAPIMemoryType memtype) {
        static DrvAPISection sections[DrvAPIMemoryType::DrvAPIMemoryNTypes];
        return sections[memtype];
    }
    
    std::atomic<uint64_t> base_; //!< base address of the section
    std::atomic<uint64_t> size_; //!< size of the section
};

/**
 * @brief Statically allocate data in the specified memory type
 */
template <typename T, DrvAPIMemoryType MEMTYPE>
class DrvAPIGlobal
{
public:
    typedef T value_type; //!< value type
    typedef DrvAPIPointer<T> pointer_type; //!< pointer type
    
    /**
     * @brief constructor
     */
    DrvAPIGlobal() {
        DrvAPISection &section = DrvAPISection::GetSection(MEMTYPE);
        pointer_ = DrvAPIPointer<T>
            (section.getBase() + section.increaseSizeBy(sizeof(T)));
    }

    // not copyable or movable
    DrvAPIGlobal(const DrvAPIGlobal &other) = delete;
    DrvAPIGlobal(DrvAPIGlobal &&other) = delete;
    DrvAPIGlobal &operator=(const DrvAPIGlobal &other) = delete;
    DrvAPIGlobal &operator=(DrvAPIGlobal &&other) = delete;
    
    
    /**
     * cast operator
     */
    operator T() const { return static_cast<T>(*pointer_); }

    /**
     * assignment operator
     */
    DrvAPIGlobal& operator=(const T &other) {
        *pointer_ = other;
        return *this;
    }

    /**
     * address operator
     */
    DrvAPIPointer<T> operator&() const { return pointer_; }
    
    DrvAPIPointer<T> pointer_; //!< pointer to the data
    T init_val_; //!< initial value
};

/**
 * @brief specialization for DrvAPIPointer globals
 */
template <typename T, DrvAPIMemoryType MEMTYPE>
class DrvAPIGlobal<DrvAPIPointer<T>, MEMTYPE>
{
public:
    /**
     * @brief constructor
     */
    DrvAPIGlobal() {
        DrvAPISection &section = DrvAPISection::GetSection(MEMTYPE);
        pointer_ = DrvAPIPointer<T>
            (section.getBase() + section.increaseSizeBy(sizeof(T)));
    }

    // not copyable or movable
    DrvAPIGlobal(const DrvAPIGlobal &other) = delete;
    DrvAPIGlobal(DrvAPIGlobal &&other) = delete;
    DrvAPIGlobal &operator=(const DrvAPIGlobal &other) = delete;
    DrvAPIGlobal &operator=(DrvAPIGlobal &&other) = delete;


    /**
     * cast operator
     */
    operator DrvAPIPointer<T>() const { return static_cast<DrvAPIPointer<T>>(*pointer_); }

    /**
     * assignment operator
     */
    DrvAPIGlobal& operator=(const DrvAPIPointer<T> &other) {
        *pointer_ = other;
        return *this;
    }

    /**
     * address operator
     */
    DrvAPIPointer<DrvAPIPointer<T>> operator&() const { return pointer_; }

    /**
     * subscript operator
     */
    typename DrvAPIPointer<T>::value_handle
    operator[](size_t idx) const {
        DrvAPIPointer<T> p = *pointer_;
        return p[idx];
    }

    DrvAPIPointer<DrvAPIPointer<T>> pointer_; //!< pointer to the data
};

template <typename T>
using DrvAPIGlobalL1SP = DrvAPIGlobal<T, DrvAPIMemoryType::DrvAPIMemoryL1SP>;

template <typename T>
using DrvAPIGlobalDRAM = DrvAPIGlobal<T, DrvAPIMemoryType::DrvAPIMemoryDRAM>;
}
#endif
