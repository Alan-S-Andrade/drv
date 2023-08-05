#pragma once
#include "DrvAPIAddress.hpp"
#include "DrvAPIMemory.hpp"
#include <cstddef>
namespace DrvAPI
{

/**
 * @brief The pointer class
 * 
 * @tparam T 
 */
template <typename T>
class DrvAPIPointer
{
public:
    typedef DrvAPIPointer<T> pointer_type; //!< pointer type
    typedef T value_type; //!< value type

    /**
     * base constructor
     */
    DrvAPIPointer(const DrvAPIAddress& vaddr) :vaddr_(vaddr) {}

    /**
     * constructor from uint64_t
     */
    DrvAPIPointer(uint64_t vaddr) : DrvAPIPointer(DrvAPIAddress(vaddr)) {}
    
    /**
     * empty constructor
     */
    DrvAPIPointer() : DrvAPIPointer(DrvAPIAddress()) {}

    /**
     * copy constructor
     */
    DrvAPIPointer(const pointer_type &other) = default;

    /**
     * move constructor
     */
    DrvAPIPointer(pointer_type &&other) = default;

    /**
     * copy assignment
     */
    pointer_type &operator=(const pointer_type &other) = default;

    /**
     * move assignment
     */
    pointer_type &operator=(pointer_type &&other) = default;

    /**
     * destructor
     */
    ~DrvAPIPointer() = default;


    /**
     * cast operator to DrvAPIAddress
     */
    operator DrvAPIAddress() const {
        return vaddr_;
    }

    /**
     * cast operator to uint64_t
     */
    operator uint64_t() const {
        return static_cast<uint64_t>(vaddr_);
    }
    
    /**
     * handle
     */
    class value_handle {
    public:
        /**
         * constructor
         */
        value_handle(const DrvAPIAddress &vaddr) :vaddr_(vaddr) {}        
        value_handle() = delete;
        value_handle(const value_handle &other) = delete;
        value_handle(value_handle &&other) = default;
        value_handle &operator=(const value_handle &other) = delete;
        value_handle &operator=(value_handle &&other) = default;
        ~value_handle() = default;

        /**
         * cast operator to type T
         */
        operator value_type() const {
            return DrvAPI::read<T>(vaddr_);
        }

        /**
         * assignment operator
         */
        value_handle &operator=(const T &value) {
            DrvAPI::write<T>(vaddr_, value);
            return *this;
        }

        /**
         * address of operator
         */
        pointer_type operator&() {
            return pointer_type(vaddr_);
        }
        
        DrvAPIAddress vaddr_;
    };
        
    /**
     * dereference operator
     */
    value_handle operator*() const {
        return value_handle(vaddr_);
    }

    /**
     * post increment operator
     */
    pointer_type operator++(int) {
        pointer_type tmp(*this);
        vaddr_ += sizeof(value_type);
        return tmp;
    }

    /**
     * pre increment operator
     */
    pointer_type &operator++() {
        vaddr_ += sizeof(value_type);
        return *this;
    }

    /**
     * post decrement operator
     */
    pointer_type operator--(int) {
        pointer_type tmp(*this);
        vaddr_ -= sizeof(value_type);
        return tmp;
    }

    /**
     * pre decrement operator
     */
    pointer_type &operator--() {
        vaddr_ -= sizeof(value_type);
        return *this;
    }

    /**
     * add assignment operator
     */
    template <typename integer_type>
    pointer_type &operator+=(integer_type rhs) {
        vaddr_ += rhs*sizeof(value_type);
        return *this;
    }

    /**
     * subtract assignment operator
     */
    template <typename integer_type>
    pointer_type &operator-=(integer_type  rhs) {
        vaddr_ -= rhs*sizeof(value_type);
        return *this;
    }


    /**
     * addition operator
     */
    template <typename integer_type>
    pointer_type operator+(integer_type rhs) const {
        pointer_type tmp(*this);
        tmp += rhs;
        return tmp;
    }

    /**
     * subtraction operator
     */
    template <typename integer_type>
    pointer_type operator-(integer_type rhs) const {
        pointer_type tmp(*this);
        tmp -= rhs;
        return tmp;
    }

    /**
     * index operator
     */
    template <typename integer_type>
    value_handle operator[](integer_type rhs) const {
        return value_handle(vaddr_ + rhs*sizeof(value_type));
    }

    /**
     * warning do not use this operator
     * use the macros instead
     */
    value_type* operator->() {
        return nullptr;
    }

    DrvAPIAddress vaddr_; //!< virtual address 
};

#define DRVAPI_POINTER_MEMBER_TYPE(ptr, member) \
    decltype(ptr->member)

#define DRVAPI_POINTER_MEMBER_POINTER(ptr, member)                      \
    (DrvAPI::DrvAPIPointer<DRVAPI_POINTER_MEMBER_TYPE(ptr,member)>      \
     ((ptr).vaddr_ + offsetof(decltype(ptr)::value_type, member)))

#define DRVAPI_POINTER_DATA_MEMBER(ptr, member) \
    (*DRVAPI_POINTER_MEMBER_POINTER(ptr, member))
    
}
