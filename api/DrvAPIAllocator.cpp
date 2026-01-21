// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include "DrvAPI.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <atomic>
#include <array>
#include <inttypes.h>
namespace DrvAPI
{
//#define DEBUG_ALLOCATOR
#ifdef DEBUG_ALLOCATOR
#define pr_dbg(fmt, ...)                        \
    do {                                        \
    printf("[ALLOCATOR: PXN %3d, POD %3d, C %3d, T %3d] " fmt "", \
           DrvAPIThread::current()->pxnId(),    \
           DrvAPIThread::current()->podId(),    \
           DrvAPIThread::current()->coreId(),   \
           DrvAPIThread::current()->threadId(), \
           ##__VA_ARGS__);                      \
    } while (0)
#else
#define pr_dbg(fmt, ...) do {} while (0)
#endif

namespace allocator {
class bump_allocator;
class slab_allocator;
class block_allocator;

typedef int64_t          lock_t;
typedef int64_t          status_t;
typedef DrvAPIAddress    address_t;
typedef DrvAPIMemoryType memtype_t;
static const pointer<void> null_ptr = -1;
}
using namespace allocator;

namespace
{

/**
 * byte definition
 */
typedef uint8_t byte;

/**
 * byte_array definition
 */
typedef std::array<byte, 8> byte_array;

/**
 * @brief a convenience class for handling bitfields
 * lets us have bitfields from value_handles
 */
template <typename UINT, uint64_t HI, uint64_t LO, unsigned TAG=0>
struct bitfield_handle {
public:
    typedef UINT uint_type;
    static constexpr uint64_t HI_BIT = HI;
    static constexpr uint64_t LO_BIT = LO;

    bitfield_handle(DrvAPI::value_handle<UINT> i) : i(i) {}
    ~bitfield_handle() = default;
    bitfield_handle(bitfield_handle &&o) = default;
    bitfield_handle &operator=(bitfield_handle &&o) = default;
    bitfield_handle(const bitfield_handle &o) = default;
    bitfield_handle &operator=(const bitfield_handle &o) = default;

    static constexpr UINT lo() {
        return LO;
    }
    static constexpr UINT hi() {
        return HI;
    }
    static constexpr UINT bits() {
        return HI - LO + 1;
    }

    static constexpr UINT mask()
    {
        return ((1ull << (HI - LO + 1ull)) - 1ull) << LO;
    }

    static UINT getbits(UINT in)
    {
        return (in & mask()) >> LO;
    }

    static void setbits(DrvAPI::value_handle<UINT> in, UINT val)
    {
        in = in & ~mask();
        in = in | ( mask() & (val << LO));
    }

    operator UINT() const { return getbits(i); }

    bitfield_handle &operator=(UINT val) {
        setbits(i, val);
        return *this;
    }

    DrvAPI::value_handle<UINT> i;
};

/**
 * @brief acquires a lock on construction, releases on destruction
 */
struct lock_guard {
public:
    lock_guard(pointer<lock_t> lock) : lock(lock) {
        int cycles = 16;
        while (atomic_cas(lock, 0, 1) != 0) {
            DrvAPI::wait(cycles);
        }
    }
    ~lock_guard() {
        DrvAPI::fence();
        *lock = 0;
    }

private:
    pointer<lock_t> lock;
};

static constexpr status_t STATUS_UNINIT = 0;
static constexpr status_t STATUS_INIT = 1;
static constexpr status_t STATUS_INIT_IN_PROCESS = 2;

/**
 * @brief status guard for doing once
 */
template <typename F>
void do_once(pointer<status_t> status, F && f) {
    status_t s = atomic_cas(status, STATUS_UNINIT, STATUS_INIT_IN_PROCESS);
    // do init; hasn't happened yet
    if (s == STATUS_UNINIT) {
        f();
        DrvAPI::fence();
        *status = s = STATUS_INIT;
    }
    // wait for init to complete
    while (s != STATUS_INIT) {
        DrvAPI::wait(32);
        s = *status;
    }
}

} // namespace

namespace allocator {
#define FIELD(type, name, field)                \
    type field;                                  \
    type &name() { return field; }               \
    const type &name() const { return field; }   \

} // namespace allocator

////////////////////
// BUMP ALLOCATOR //
////////////////////
namespace allocator {

/**
 * @brief uses atomic add to allocate memory
 */
class bump_allocator {
public:
    FIELD(address_t, base, base_)
    FIELD(address_t, end,  end_)
    FIELD(status_t, status, status_)
    template <typename Dst, typename Src>
    static void copy(Dst &dst, const Src &src) {
        dst.base() = src.base();
        dst.end() = src.end();
        dst.status() = src.status();
    }
};

}

template <>
class value_handle<bump_allocator> {
    DRV_API_VALUE_HANDLE_DEFAULTS(bump_allocator)
    DRV_API_VALUE_HANDLE_FIELD(bump_allocator, base, address_t, base_)
    DRV_API_VALUE_HANDLE_FIELD(bump_allocator, end, address_t,  end_)
    DRV_API_VALUE_HANDLE_FIELD(bump_allocator, status, status_t, status_)
    /**
     * @brief initialize the allocator
     */
    void init(address_t base, address_t size) {
        do_once(status().address(), [this, base, size](){
            this->base() = base;
            this->end() = base + size;
        });
    }

    /**
     * allocate a slab of memory
     */
    pointer<void> allocate(address_t size) {
        using namespace allocator;
        // align to 8-byte boundary
        size = (size + 7) & ~7;
        address_t addr = DrvAPI::atomic_add<address_t>(base().address(), size);
        pr_dbg("bump allocator allocated %s (%10lx + size = %lu)\n",
               DrvAPIVAddress{addr}.to_string().c_str(),
               addr,
               size);
        if (addr + size > this->end()) {
            pr_dbg("bump allocator returning null pointer\n");
            return allocator::null_ptr;
        }

        return pointer<void>(addr);
    }

    /**
     * reset the allocator
     */
    void reset(address_t base, address_t size) {
        // mark as uninitialized (only if still marked as initialized)
        atomic_cas(status().address(), STATUS_INIT, STATUS_UNINIT);
        do_once(status().address(), [this, base, size](){
            this->base() = base;
            this->end() = base + size;
        });
    }
};

////////////////////
// SLAB ALLOCATOR //
////////////////////
namespace allocator {
class slab_allocator {
public:
    FIELD(bump_allocator, bump_alloc, bump_alloc_)
    template <typename Dst, typename Src>
    static void copy(Dst &dst, const Src &src) {
        dst.bump_alloc() = src.bump_alloc();
    }
};
} // namespace allocator

template <>
class value_handle<slab_allocator> {
    DRV_API_VALUE_HANDLE_DEFAULTS(slab_allocator)
    DRV_API_VALUE_HANDLE_FIELD(slab_allocator, bump_alloc, allocator::bump_allocator, bump_alloc_)
    void init(memtype_t type) {
        using namespace allocator;
        DrvAPISection &section = DrvAPI::DrvAPISection::GetSection(type);
        address_t sz = static_cast<address_t>(section.getSize());
        // align to 16-byte boundary
        sz = (sz + 15) & ~15;
        // make a global address
        address_t localBase = section.getBase(myPXNId(), myPodId(), myCoreId());
        address_t globalBase = toAbsoluteAddress(localBase);
        bump_alloc().init(globalBase+sz, getMemSize(type)-sz);
    }

    address_t getMemSize(memtype_t type) {
        switch (type) {
        case DrvAPIMemoryL1SP:
            return DrvAPI::coreL1SPSize();
        case DrvAPIMemoryL2SP:
            return DrvAPI::podL2SPSize();
        case DrvAPIMemoryDRAM:
            return DrvAPI::pxnDRAMSize();
        default:
            throw std::runtime_error("slab_allocator: unknown memory type");
        }
    }
    
    /**
     * allocate a slab of memory
     */
    pointer<void> allocate(address_t size) {
        pointer<void>r = bump_alloc().allocate(size);
        if (r == allocator::null_ptr) {
            throw std::runtime_error("slab_allocator: out of memory");
        }
        return r;
    }
};

/////////////////////
// BLOCK ALLOCATOR //
/////////////////////
namespace allocator {
/**
 * @brief block of memory
 */
struct block {
    FIELD(uint64_t,       info, info_);
    FIELD(pointer<block>, next, next_);
    FIELD(pointer<block>, prev, prev_);
    FIELD(byte_array,     data, data_);

    template <typename Src, typename Dst>
    static void copy(Dst &dst, const Src &src) {
        dst.info() = src.info();
        dst.next() = src.next();
        dst.prev() = src.prev();
    }
};

/**
 * @brief allocator
 */
class block_allocator {
public:
    FIELD(pointer<block>, free_list, free_list_);
    FIELD(int64_t,        lock, lock_);
    FIELD(status_t,       status, status_);
    FIELD(pointer<slab_allocator>, slab_alloc_ptr, slab_alloc_ptr_);

    template <typename Src, typename Dst>
    static void copy(Dst &dst, const Src &src) {
        dst.free_list() = src.free_list();
        dst.lock() = src.lock();
        dst.status() = src.status();
        dst.slab_alloc_ptr() = src.slab_alloc_ptr();
    }
};
} // namespace allocator

/**
 * specialization of byte_array
 */
template <>
class value_handle<byte_array> {
    DRV_API_VALUE_HANDLE_DEFAULTS_TRIVIAL(byte_array)
    value_handle<byte> operator[](size_t index) {
        return value_handle<byte>(address() + index);
    }
    const value_handle<byte> operator[](size_t index) const {
        return value_handle<byte>(address() + index);
    }
};

/**
 * specialization for block
 */
template <>
class value_handle<block> {
    DRV_API_VALUE_HANDLE_DEFAULTS(block)
    DRV_API_VALUE_HANDLE_FIELD(block, info, uint64_t, info_) // {is_free, is_predecessor_free, size}
    DRV_API_VALUE_HANDLE_FIELD(block, next, pointer<block>, next_) // next free block (only valid if this block is free)
    DRV_API_VALUE_HANDLE_FIELD(block, prev, pointer<block>, prev_) // previous free block (only valid if this block is free)
    DRV_API_VALUE_HANDLE_FIELD(block, data, byte_array, data_) // block payload data
    /**
     * @brief is_free
     *
     * set to true if the block is free
     */
    bitfield_handle<uint64_t, 0, 0> is_free() {
        return bitfield_handle<uint64_t, 0, 0>(info());
    }
    /**
     * @brief is_free
     *
     * set to true if the block is free
     */
    const bitfield_handle<uint64_t, 0, 0> is_free() const {
        return bitfield_handle<uint64_t, 0, 0>(info());
    }
    /**
     * @brief is_predecessor_free
     *
     * set to true if the predecessor block is free (block immediately adjacent to the left)
     */
    bitfield_handle<uint64_t, 1, 1> is_predecessor_free() {
        return bitfield_handle<uint64_t, 1, 1>(info());
    }
    /**
     * @brief is_predecessor_free
     *
     * set to true if the predecessor block is free (block immediately adjacent to the left)
     */
    const bitfield_handle<uint64_t, 1, 1> is_predecessor_free() const {
        return bitfield_handle<uint64_t, 1, 1>(info());
    }
    /**
     * @brief size
     *
     * size of the block
     */
    bitfield_handle<uint64_t, 63, 2> size() {
        return bitfield_handle<uint64_t, 63, 2>(info());
    }
    /**
     * @brief size
     *
     * size of the block
     */
    const bitfield_handle<uint64_t, 63, 2> size() const {
        return bitfield_handle<uint64_t, 63, 2>(info());
    }
    /**
     * @brief handle to the footer of this block
     */
    value_handle<uint64_t> footer() {
        return value_handle<uint64_t>(address() + size() - sizeof(uint64_t));
    }
    /**
     * @brief handle to the footer of this block
     */
    const value_handle<uint64_t> footer() const {
        return value_handle<uint64_t>(address() + size() - sizeof(uint64_t));
    }
    /**
     * @brief handle to this block's successor
     */
    value_handle<block> successor() {
        return value_handle<block>(address() + size());
    }
    /**
     * @brief handle to this block's successor
     */
    const value_handle<block> successor() const {
        return value_handle<block>(address() + size());
    }
    /**
     * @brief the predecessor's size (uses the predecessor's footer)
     */
    const value_handle<uint64_t> predecessor_size() const {
        return value_handle<uint64_t>(address() - sizeof(uint64_t));
    }
    /**
     * @brief handle to this block's predecessor
     */
    value_handle<block> predecessor() {
        return value_handle<block>(address() - (uint64_t)predecessor_size());
    }
    /**
     * @brief handle to this block's predecessor
     */
    const value_handle<block> predecessor() const {
        return value_handle<block>(address() - (uint64_t)predecessor_size());
    }
};




/**
 * specialization of the value_handle for cello::allocator
 */
template <>
class value_handle<block_allocator> {
    DRV_API_VALUE_HANDLE_DEFAULTS(block_allocator)
    DRV_API_VALUE_HANDLE_FIELD(block_allocator, free_list, pointer<block>, free_list_)
    DRV_API_VALUE_HANDLE_FIELD(block_allocator, lock, int64_t, lock_)
    DRV_API_VALUE_HANDLE_FIELD(block_allocator, slab_alloc_ptr, pointer<slab_allocator>, slab_alloc_ptr_)
    DRV_API_VALUE_HANDLE_FIELD(block_allocator, status, status_t, status_)

    /**
     * return true if the free list is empty
     */
    bool empty() const {
        pointer<block> free_list_ptr = free_list();
        value_handle<block> free_list = *free_list_ptr;
        if (!free_list.is_free()) {
            return true;
        }
        return false;
    }

    /**
     * @brief initialize the block allocator
     */
    void init(pointer<slab_allocator> slab) {
        do_once(status().address(), [this,slab]() mutable {
            this->slab_alloc_ptr() = slab;
            this->free_list() = new_block(1024);
            this->lock() = 0;
        });
    }

    /**
     * @brief use the slab allocator to init a new block
     */
    pointer<block> new_block(uint64_t size) {
        pointer<slab_allocator> slab = slab_alloc_ptr();
        // +1 word for the info field
        pointer<block> block_ptr = slab->allocate(size + sizeof(uint64_t));
        auto block = *block_ptr;
        block.size() = size;
        block.is_free() = true;
        block.is_predecessor_free() = false;
        block.footer() = size;
        block.next() = block.address();
        block.prev() = block.address();
        return block.address();
    }

private:
    void print_free_list(const char *str) {
#ifdef DEBUG_ALLOCATOR
        if (empty())
            return;

        pointer<block> curr_block_ptr = free_list();
        pr_dbg("%s: free_list:\n", str);
        do {
            auto curr_block = *curr_block_ptr;
            pr_dbg("%s: {%s}->{size()=%lu,is_free()=%lu,is_predecessor_free()=%lu}\n",
                   str,
                   DrvAPIVAddress{curr_block.address()}.to_string().c_str(),
                   (address_t)curr_block.size(),
                   (address_t)curr_block.is_free(),
                   (address_t)curr_block.is_predecessor_free());
            curr_block_ptr = curr_block.next();
        } while (curr_block_ptr != free_list());
#endif
    }

public:
    /**
     * @brief allocate a block of memory
     */
    pointer<void> allocate(address_t size) {
        // align the size to 8 bytes
        size = (size + 7) & ~7;
        // make sure at least min size
        size = std::max(size, sizeof(block));
        // add one word for the info field
        size += sizeof(uint64_t);

        lock_guard guard(lock().address());

    scan_free_list:
        if (!empty()) {
            print_free_list("allocate call   ");
            pointer<block> curr_block_ptr = free_list();
            do {
                auto curr_block = *curr_block_ptr;
                if (curr_block.is_free() &&
                    curr_block.size() >= size) {
                    if (curr_block.size() > size + sizeof(block) + sizeof(uint64_t)) {
                        // split the block
                        // create a new block with the remaining size
                        pointer<block> new_block_ptr = curr_block.address() + size;
                        auto new_block = *new_block_ptr;
                        new_block.size() = curr_block.size() - size;
                        new_block.is_free() = true;
                        new_block.is_predecessor_free() = false;
                        new_block.footer() = new_block.size();
                        // update the current block
                        curr_block.size() = size;
                        curr_block.footer() = size;
                        // update the free list
                        pointer<block> next_block_ptr = curr_block.next();
                        auto next_block = *next_block_ptr;
                        pointer<block> prev_block_ptr = curr_block.prev();
                        auto prev_block = *prev_block_ptr;
                        if (curr_block.address() == next_block.address()) {
                            // corner case: the free list is a single block
                            new_block.next() = new_block.address();
                            new_block.prev() = new_block.address();
                        } else {
                            new_block.next() = next_block.address();
                            new_block.prev() = prev_block.address();
                            next_block.prev() = new_block.address();
                            prev_block.next() = new_block.address();
                        }
                        // point the free list to the new block
                        free_list() = new_block.address();
                        // unmark this block as not free
                        curr_block.is_free() = false;
                        print_free_list("allocate ret0   ");
                        return curr_block.next().address();
                    } else {
                        // remove the block from the free list
                        pointer<block> next_block_ptr = curr_block.next();
                        auto next_block = *next_block_ptr;
                        pointer<block> prev_block_ptr = curr_block.prev();
                        auto prev_block = *prev_block_ptr;
                        next_block.prev() = prev_block.address();
                        prev_block.next() = next_block.address();
                        free_list() = next_block.address();
                        // update the successor
                        curr_block.successor().is_predecessor_free() = false;
                        // mark this block as not free
                        curr_block.is_free() = false;
                        // return the payload
                        print_free_list("allocate ret1   ");
                        return curr_block.next().address();
                    }
                }
                curr_block_ptr = curr_block.next();
            } while (curr_block_ptr != free_list());
        }
        // allocate a new slab
        pointer<block> slab_ptr = new_block(size*2);
        auto slab = *slab_ptr;
        pr_dbg("allocate newslab: {%s}->{size()=%lu,is_free()=%lu,is_predecessor_free()=%lu}\n",
               DrvAPIVAddress{slab.address()}.to_string().c_str(),
               (address_t)slab.size(),
               (address_t)slab.is_free(),
               (address_t)slab.is_predecessor_free());

        if (empty()) {
            free_list() = slab.address();
        } else {
            pointer<block> next_ptr = free_list();
            auto next = *next_ptr;
            pointer<block> prev_ptr = next.prev();
            auto prev = *prev_ptr;
            slab.next() = next.address();
            slab.prev() = prev.address();
            next.prev() = slab.address();
            prev.next() = slab.address();
        }

        // try again
        goto scan_free_list;

        return pointer<void>{-1u};
    }

    void deallocate(pointer<void> ptr) {
        pointer<block> free_block_ptr = ptr - offsetof(block, next_);
        auto free_block = *free_block_ptr;
        lock_guard guard(lock().address());
        print_free_list("deallocate call ");
        if (!free_block.is_predecessor_free()) {
            if (!free_block.successor().is_free()) {
                // do not coalesce
                free_block.is_free() = true;
                free_block.successor().is_predecessor_free() = true;
                if (empty()) {
                    free_block.next() = free_block.address();
                    free_block.prev() = free_block.address();
                    free_list() = free_block.address();
                } else {
                    pointer<block> next_ptr = free_list();
                    auto next = *next_ptr;
                    pointer<block> prev_ptr = next.prev();
                    auto prev = *prev_ptr;
                    free_block.next() = next.address();
                    free_block.prev() = prev.address();
                    next.prev() = free_block.address();
                    prev.next() = free_block.address();
                }
                print_free_list("deallocate ret0 ");
            } else {
                // coalesce with successor
                pointer<block> successor_ptr = free_block.successor().address();
                auto successor = *successor_ptr;
                free_block.size() = free_block.size() + successor.size();
                free_block.is_free() = true;
                free_block.footer() = free_block.size();
                pointer<block> next_ptr = successor.next();
                auto next = *next_ptr;
                pointer<block> prev_ptr = successor.prev();
                auto prev = *prev_ptr;
                if (next.address() == successor.address()) {
                    // corner case: the free list is a single block
                    free_block.next() = free_block.address();
                    free_block.prev() = free_block.address();
                } else {
                    free_block.next() = next.address();
                    free_block.prev() = prev.address();
                    next.prev() = free_block.address();
                    prev.next() = free_block.address();
                }
                free_list() = free_block.address();
                print_free_list("deallocate ret1 ");
            }
        } else {
            if (!free_block.successor().is_free()) {
                // coalesce with predecessor
                pointer<block> predecessor_ptr = free_block.predecessor().address();
                auto predecessor = *predecessor_ptr;
                predecessor.size() = predecessor.size() + free_block.size();
                predecessor.footer() = predecessor.size();
                predecessor.successor().is_predecessor_free() = true;
                print_free_list("deallocate ret2 ");
            } else {
                // coalesce with predecessor and successor
                pointer<block> predecessor_ptr = free_block.predecessor().address();
                auto predecessor = *predecessor_ptr;
                pointer<block> successor_ptr = free_block.successor().address();
                auto successor = *successor_ptr;
                // remove successor from free list
                pointer<block> next_ptr = successor.next();
                auto next = *next_ptr;
                pointer<block> prev_ptr = successor.prev();
                auto prev = *prev_ptr;
                next.prev() = prev.address();
                prev.next() = next.address();
                // coalesce
                predecessor.size() = predecessor.size() + free_block.size() + successor.size();
                predecessor.footer() = predecessor.size();
                free_list() = predecessor.address();
                print_free_list("deallocate ret3 ");
            }
        }
    }
};

//////////////////////////
// FIXED SIZE ALLOCATOR //
//////////////////////////
namespace allocator {
/**
 * allocator for fixed size objects
 */
template <address_t SIZE>
struct object {
    static constexpr address_t DWORDS = (SIZE + sizeof(address_t) - 1) / sizeof(address_t);
    union {
        pointer<object> next_;
        std::array<address_t, DWORDS> data_;
    };
    const pointer<object> &next() const { return next_; }
    pointer<object> &next() { return next_; }
    const std::array<address_t, DWORDS> &data() const { return data_; }
    std::array<address_t, DWORDS> &data() { return data_; }

    template <typename Dst, typename Src>
    static void copy(Dst &dst, const Src &src) {
        dst.next() = src.next();
        dst.data() = src.data();
    }
};

/**
 * allocator for fixed size objects with free list
 */
template <address_t SIZE>
class free_list_object_allocator {
public:
    FIELD(pointer<object<SIZE>>, head, head_)
    FIELD(status_t, status, status_)
    template <typename Dst, typename Src>
    static void copy(Dst &dst, const Src &src) {
        dst.head() = src.head();
    }
};

/**
 * fixed size object allocator with free list and bump
 */
template <address_t SIZE>
class object_allocator {
public:
    FIELD(free_list_object_allocator<SIZE>, free_list, free_list_)
    FIELD(bump_allocator, bump, bump_)
    FIELD(pointer<slab_allocator>, slab_alloc_ptr, slab_alloc_ptr_)
    FIELD(status_t, status, status_)
    template <typename Dst, typename Src>
    static void copy(Dst &dst, const Src &src) {
        dst.free_list() = src.free_list();
        dst.bump() = src.bump();
        dst.slab_alloc_ptr() = src.slab_alloc_ptr();
    }    
};
}

/**
 * specialization for object
 */
template <address_t SIZE>
class value_handle<allocator::object<SIZE>> {
    DRV_API_VALUE_HANDLE_DEFAULTS(allocator::object<SIZE>)
    DRV_API_VALUE_HANDLE_FIELD(allocator::object<SIZE>, next, pointer<allocator::object<SIZE>>, next_)
};

/**
 * specialization for free_list_object_allocator
 */
template <address_t SIZE>
class value_handle<allocator::free_list_object_allocator<SIZE>> {
    typedef pointer<allocator::object<SIZE>> object_pointer;
    DRV_API_VALUE_HANDLE_DEFAULTS(allocator::free_list_object_allocator<SIZE>)
    DRV_API_VALUE_HANDLE_FIELD(allocator::free_list_object_allocator<SIZE>, head, pointer<allocator::object<SIZE>>, head_)
    void init() {
        head() = head().address();
        pr_dbg("free_list allocator: init: &head = %s, head = %s\n",
               DrvAPIVAddress{head().address()}.to_string().c_str(),
               DrvAPIVAddress{head()}.to_string().c_str());
    }

    bool empty() const {
        pointer<void> head_ptr = head();
        return head_ptr == head().address();
    }

    object_pointer allocate() {
        while (true) {
            object_pointer head_ptr = head();
            object_pointer next_ptr = head_ptr->next();
            pr_dbg("free_list allocator: allocate: &head = %s, head = %s, next = %s\n",
                   DrvAPIVAddress{head().address()}.to_string().c_str(),
                   DrvAPIVAddress{head_ptr}.to_string().c_str(),
                   DrvAPIVAddress{next_ptr}.to_string().c_str());
            object_pointer result = atomic_cas(head().address(), head_ptr, next_ptr);
            pr_dbg("free_list allocator: allocate: &head = %s, allocated %s\n",
                   DrvAPIVAddress{head().address()}.to_string().c_str(),
                   DrvAPIVAddress{result}.to_string().c_str());
            
            if (result == head().address())
                return allocator::null_ptr;
            else if (result == head_ptr)
                return head_ptr;
        }
    }
    
    void deallocate(object_pointer ptr) {
        while (true) {
            object_pointer head_ptr = head();
            ptr->next() = head_ptr;
            object_pointer result = atomic_cas(head().address(), head_ptr, ptr);
            if (result == head_ptr)
                return;
        }
    }
};

template <address_t SIZE>
class value_handle<allocator::object_allocator<SIZE>> {
    typedef pointer<allocator::object<SIZE>> object_pointer;    
    DRV_API_VALUE_HANDLE_DEFAULTS(allocator::object_allocator<SIZE>)
    DRV_API_VALUE_HANDLE_FIELD(allocator::object_allocator<SIZE>, free_list, allocator::free_list_object_allocator<SIZE>, free_list_)
    DRV_API_VALUE_HANDLE_FIELD(allocator::object_allocator<SIZE>, bump, bump_allocator, bump_)
    DRV_API_VALUE_HANDLE_FIELD(allocator::object_allocator<SIZE>, slab_alloc_ptr, pointer<slab_allocator>, slab_alloc_ptr_)
    DRV_API_VALUE_HANDLE_FIELD(allocator::object_allocator<SIZE>, status, status_t, status_)
    static constexpr address_t SLAB_SIZE = SIZE * 32;
    void init(pointer<slab_allocator> slab_alloc_ptr) {
        do_once(status().address(), [slab_alloc_ptr, this] {
            this->slab_alloc_ptr() = slab_alloc_ptr;
            this->free_list().init();
            this->bump().init(0,0);
        });
    }

    object_pointer allocate() {
        object_pointer ptr;
        ptr = bump().allocate(SIZE);
        if (ptr != allocator::null_ptr)
            return ptr;

        ptr = free_list().allocate();
        if (ptr != allocator::null_ptr)
            return ptr;

        pointer<slab_allocator> slab_alloc_ptr = this->slab_alloc_ptr();
        bump().reset(slab_alloc_ptr->allocate(SLAB_SIZE), SLAB_SIZE);
        ptr = bump().allocate(SIZE);
        if (ptr != allocator::null_ptr)
            return ptr;

        throw std::runtime_error("object allocator: out of memory");
    }

    void deallocate(pointer<allocator::object<SIZE>> ptr) {
        free_list().deallocate(ptr);
    }
};

///////////////////////////
// GLOBAL MEMORY OBJECTS //
///////////////////////////
namespace allocator {
/**
 * global memory allocator
 */
struct global_memory {
    FIELD(slab_allocator, slab_alloc, slab_alloc_)
    FIELD(block_allocator, block_alloc, block_alloc_)
    FIELD(object_allocator<sizeof(uint64_t)>, dword_alloc, dword_alloc_)
    FIELD(object_allocator<2*sizeof(uint64_t)>, qword_alloc, qword_alloc_)    
    template <typename Dst, typename Src>
    static void copy(Dst &dst, const Src &src) {
        dst.slab_alloc() = src.slab_alloc();
        dst.block_alloc() = src.block_alloc();
        dst.dword_alloc() = src.dword_alloc();
        dst.qword_alloc() = src.qword_alloc();
    }
};
} // namespace allocator

template <>
class value_handle<global_memory> {
    DRV_API_VALUE_HANDLE_DEFAULTS(global_memory)
    DRV_API_VALUE_HANDLE_FIELD(global_memory, slab_alloc, slab_allocator, slab_alloc_)
    DRV_API_VALUE_HANDLE_FIELD(global_memory, block_alloc, block_allocator, block_alloc_)
    DRV_API_VALUE_HANDLE_FIELD(global_memory, dword_alloc, object_allocator<sizeof(uint64_t)>, dword_alloc_)
    DRV_API_VALUE_HANDLE_FIELD(global_memory, qword_alloc, object_allocator<2*sizeof(uint64_t)>, qword_alloc_)
    void init(memtype_t type) {
        slab_alloc().init(type);
        block_alloc().init(slab_alloc().address());
        dword_alloc().init(slab_alloc().address());
        qword_alloc().init(slab_alloc().address());
    }

    pointer<void> allocate(address_t size) {
#define BUMP_ALLOCATE_ONLY
#ifdef  BUMP_ALLOCATE_ONLY
        return slab_alloc().allocate(size);
#else
        if (size <= sizeof(uint64_t))
            return dword_alloc().allocate();
        else if (size <= 2*sizeof(uint64_t))
            return qword_alloc().allocate();
        else
            return block_alloc().allocate(size);
#endif
    }

    void deallocate(pointer<void> ptr, address_t size) {
#ifdef BUMP_ALLOCATE_ONLY
#else
        if (size <= sizeof(uint64_t))
            dword_alloc().deallocate(ptr);
        else if (size <= 2*sizeof(uint64_t))
            qword_alloc().deallocate(ptr);
        else
            block_alloc().deallocate(ptr);
#endif
    }
};

namespace allocator {
/**
 * L1SP memory allocator
 */
l1sp_static<global_memory> l1sp_memory; // one of these per core

/**
 * L2SP memory allocator
 */
l2sp_static<global_memory> l2sp_memory; // one of these per pod

/**
 * DRAM memory allocator
 */
dram_static<global_memory> dram_memory; // one of these per pxn

} // namespace allocator

/////////
// API //
/////////
void DrvAPIMemoryAllocatorInitType(DrvAPIMemoryType type) {
    if (isCommandProcessor()) {
        if (type == DrvAPIMemoryType::DrvAPIMemoryL1SP) {
            std::cout << "Initializing L1SP memory allocator" << std::endl;
            l1sp_memory.init(DrvAPIMemoryType::DrvAPIMemoryL1SP);
        } else if (type == DrvAPIMemoryType::DrvAPIMemoryL2SP) {
            l2sp_memory.init(DrvAPIMemoryType::DrvAPIMemoryL2SP);
        }
    } else if (type == DrvAPIMemoryType::DrvAPIMemoryDRAM) {
        dram_memory.init(DrvAPIMemoryType::DrvAPIMemoryDRAM);
    }
}

void DrvAPIMemoryAllocatorInit() {
    DrvAPIMemoryAllocatorInitType(DrvAPIMemoryType::DrvAPIMemoryL1SP);
    DrvAPIMemoryAllocatorInitType(DrvAPIMemoryType::DrvAPIMemoryL2SP);
    DrvAPIMemoryAllocatorInitType(DrvAPIMemoryType::DrvAPIMemoryDRAM);
}

DrvAPIPointer<void> DrvAPIMemoryAlloc(DrvAPIMemoryType type, size_t size) {
    // if we use l1sp for stack, disallow allocation of l1sp
    if (type == DrvAPIMemoryType::DrvAPIMemoryL1SP && DrvAPIThread::current()->stackInL1SP()) {
        std::cerr << "ERROR: cannot allocate, L1SP memory for stack" << std::endl;
        exit(1);
    }

    // size should be 8-byte aligned
    switch (type) {
    case DrvAPIMemoryType::DrvAPIMemoryL1SP:
        return l1sp_memory.allocate(size);
    case DrvAPIMemoryType::DrvAPIMemoryL2SP:
        return l2sp_memory.allocate(size);
    case DrvAPIMemoryType::DrvAPIMemoryDRAM:
        return dram_memory.allocate(size);
    default:
        std::cerr << "ERROR: invalid memory type: " << static_cast<int>(type) << std::endl;
        exit(1);
    }
    return {0};
}

void DrvAPIMemoryFree(const DrvAPIPointer<void> &ptr, size_t size) {
    DrvAPIAddressInfo info = decodeAddress(ptr);
    if (info.is_l1sp()) {
        l1sp_memory.deallocate(ptr, size);
    } else if (info.is_l2sp()) {
        l2sp_memory.deallocate(ptr, size);
    } else if (info.is_dram()) {
        dram_memory.deallocate(ptr, size);
    } else {
        std::cerr << "ERROR: invalid memory address: " << ptr << std::endl;
        exit(1);
    }
}


}
