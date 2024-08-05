// SPDX-License-Identifier: MIT
// Copyright (c) 2024 University of Washington

#ifndef DRV_API_SECTION_H
#define DRV_API_SECTION_H
#include <DrvAPIPointer.hpp>
#include <DrvAPIInfo.hpp>
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
    DrvAPISection() : size_(0) {}

    DrvAPISection(const DrvAPISection &other) = delete;
    DrvAPISection(DrvAPISection &&other) = delete;
    DrvAPISection &operator=(const DrvAPISection &other) = delete;
    DrvAPISection &operator=(DrvAPISection &&other) = delete;
    virtual ~DrvAPISection() = default;

    /**
     * @brief get the section base
     */
    virtual uint64_t
    getBase(uint32_t pxn, uint32_t pod, uint32_t core) const = 0;

    /**
     * @brief set the section base
     */
    virtual void
    setBase(uint64_t base, uint32_t pxn, uint32_t pod, uint32_t core) = 0;

    /**
     * @brief get the section size
     */
    uint64_t getSize() const { return size_; };

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
        return size_.fetch_add(incr_size);
    }

    /**
     * @brief get the section of the specified memory type
     */
    static DrvAPISection &
    GetSection(DrvAPIMemoryType memtype);

protected:
    std::atomic<uint64_t> size_; //!< size of the section
};
}
#endif
