// SPDX-License-Identifier: MIT
// Copyright (c) 2024 University of Washington
#ifndef DRV_API_POINTER_H
#define DRV_API_POINTER_H
#include <DrvAPIAddress.hpp>
#include <DrvAPIMemory.hpp>
#include <DrvAPIAddressToNative.hpp>
#include <cstddef>
namespace DrvAPI
{
/**
 * forward declaration of pointer
 */
template <typename T>
class pointer;

/**
 * @brief default value_handle suitable for most builtin types
 */
template <typename T>
class value_handle {
public:
    value_handle(DrvAPI::DrvAPIAddress ptr):
        _ptr(ptr) {
    }
    value_handle():
        _ptr(0) {
    }    

    value_handle(const value_handle&o) {
        _ptr = o.address();
    }
    
    value_handle(value_handle && o) {
        _ptr = o.address();
    }

    value_handle & operator=(const value_handle &other) {
        *this = (T)other;
        return *this;
    }

    value_handle & operator=(value_handle &&other) {
        *this = (T)other;
        return *this;
    }

    virtual ~value_handle() = default;

    operator T() const {
        return DrvAPI::read<T>(address());        
    }

    value_handle & operator=(const T&v) {
        DrvAPI::write<T>(address(), v);
        return *this;
    }    

    pointer<T> operator&() {
        return pointer<T>(address());
    }

    virtual DrvAPI::DrvAPIAddress address() const {
        return _ptr;
    }

    const T get() const { return (T)*this; }

    T get() { return (T)*this; }

    DrvAPI::DrvAPIAddress _ptr;
};

/**
 * generate default constructors for value_handle specializations
 */
#define DRV_API_VALUE_HANDLE_CONSTRUCTORS(type)             \
    public:                                             \
        value_handle(DrvAPI::DrvAPIAddress ptr):        \
            _ptr(ptr) {                                 \
        }                                               \
        value_handle():                                 \
            _ptr(0) {                                   \
        }                                               \
        value_handle(const value_handle& o) {           \
            _ptr = o.address();                         \
        }                                               \
        value_handle(value_handle && o) {               \
            _ptr = o.address();                         \
        }                                               \
        virtual ~value_handle() = default;               \

/**
 * generate default assignment operators for value_handle specializations
 */
#define DRV_API_VALUE_HANDLE_ASSIGNMENT_OPERATORS(type)      \
    public:                                             \
        value_handle & operator=(const value_handle &other) { \
            *this = (type)other;                \
            return *this;                       \
        }                                       \
        value_handle & operator=(value_handle &&other) { \
            *this = (type)other;                \
            return *this;                       \
        }                                       \
        value_handle & operator=(const type&v) { \
            type::copy(*this, v);               \
            return *this;                       \
        }                                       \

/**
 * generate default assignment operators for value_handle specializations
 * for scalar types
 */
#define DRV_API_VALUE_HANDLE_ASSIGNMENT_OPERATORS_TRIVIAL(type)      \
    public:                                             \
        value_handle & operator=(const value_handle &other) { \
            *this = (type)other;                                  \
            return *this;                       \
        }                                       \
        value_handle & operator=(value_handle &&other) { \
             *this = (type)other;                \
             return *this;                       \
        }                                       \
        value_handle & operator=(const type&v) { \
            DrvAPI::write<type>(address(), v);      \
            return *this;                       \
        }

/**
 * generate default cast operators for value_handle specializations
 */
#define DRV_API_VALUE_HANDLE_CAST_OPERATORS(type)             \
    public:                                             \
        operator type() const {                 \
            type r;                             \
            type::copy(r, *this);               \
            return r;                           \
        }

/**
 * generate default cast operators for value_handle specializations
 * for scalar types
 */
#define DRV_API_VALUE_HANDLE_CAST_OPERATORS_TRIVIAL(type)            \
    public:                                                             \
    operator type() const {                                             \
      return DrvAPI::read<type>(address());				\
    }

/**
 * generate default addressof operators for value_handle specializations
 */
#define DRV_API_VALUE_HANDLE_ADDRESSOF_OPERATORS(type)         \
    public:                                             \
        pointer<type> operator&() {             \
            return pointer<type>(address());        \
        }

/**
 * generate the internal members for value_handle specializations
 */
#define DRV_API_VALUE_HANDLE_INTERNAL(type)          \
    public:                                             \
    virtual DrvAPI::DrvAPIAddress address() const {     \
        return _ptr;                                    \
    }                                                   \
    DrvAPI::DrvAPIAddress _ptr;

#define DRV_API_VALUE_HANDLE_DEFAULTS(type)	\
  DRV_API_VALUE_HANDLE_CONSTRUCTORS(type)	\
  DRV_API_VALUE_HANDLE_ASSIGNMENT_OPERATORS(type) \
  DRV_API_VALUE_HANDLE_CAST_OPERATORS(type)	  \
  DRV_API_VALUE_HANDLE_ADDRESSOF_OPERATORS(type)  \
  DRV_API_VALUE_HANDLE_INTERNAL(type)

#define DRV_API_VALUE_HANDLE_DEFAULTS_TRIVIAL(type)	  \
  DRV_API_VALUE_HANDLE_CONSTRUCTORS(type)		  \
  DRV_API_VALUE_HANDLE_ASSIGNMENT_OPERATORS_TRIVIAL(type) \
  DRV_API_VALUE_HANDLE_CAST_OPERATORS_TRIVIAL(type)	  \
  DRV_API_VALUE_HANDLE_ADDRESSOF_OPERATORS(type)	  \
  DRV_API_VALUE_HANDLE_INTERNAL(type)

/**
 * begins specialization of value_handle for a type
 * type - the type to specialize for
 *
 * it is expected that type defines static member functions
 * of the following forms:
 *
 * static void copy(DstType& &dst, const SrcType &src);
 * for DstType = [type, value_handle<type>] and SrcType = [type, value_handle<type>]
 */
#define DRV_API_VALUE_HANDLE_BEGIN(type)	\
  template <>					\
    class DrvAPI::value_handle<type> {		\
    DRV_API_VALUE_HANDLE_DEFAULTS(type)

/**
 * generates accessors for data members
 * type - the type of the object for which this is a handle
 * field - the name of the field, will be accessed by value_handle<type>::field()
 * field_type - the type of the field
 * field_data - the data member of the field, this must be a concreete data member inside the type
 */
#define DRV_API_VALUE_HANDLE_FIELD(type, field, field_type, field_data) \
    value_handle<field_type> field() {                                  \
        return value_handle<field_type>(address() + offsetof(type, field_data)); \
    }                                                                   \
    const value_handle<field_type> field() const {                      \
        return value_handle<field_type>(address() + offsetof(type, field_data)); \
    }


/**
 * ends specialization of value_handle for a type
 */
#define DRV_API_VALUE_HANDLE_END(type)           \
    };

/**
 * The pointer class
 *
 * This class is used to represent a pointer to a value in the target process
 * supports dereferencing and array indexing
 *
 * To get support for the -> operator, use the value_handle class
 * and the helper macros to specialize it for your type
 */
template <typename T>
class pointer {
public:
    pointer(DrvAPI::DrvAPIAddress ptr):
        _ptr(ptr) {
    }

    pointer():
        _ptr(0) {
    }

    pointer(const pointer &other) = default;
    pointer(pointer &&other) = default;
    pointer & operator=(const pointer &other) = default;
    pointer & operator=(pointer &&other) = default;
    ~pointer() = default;

    template <typename O>
    pointer(const pointer<O> &other) {
      _ptr = other._ptr;
    }

    template <typename O>
    pointer(pointer<O> && other) {
      _ptr = other._ptr;
    }

    operator DrvAPI::DrvAPIAddress() const {
        return _ptr;
    }

    value_handle<T> operator*() {
        value_handle<T> handle(_ptr);
        return handle;
    }

    const value_handle<T> operator*() const {
        value_handle<T> handle(_ptr);
        return handle;
    }

    value_handle<T> operator[](size_t index) {
        return value_handle<T>(_ptr + index * sizeof(T));
    }

    const value_handle<T> operator[](size_t index) const {
        return value_handle<T>(_ptr + index * sizeof(T));
    }

    std::unique_ptr<value_handle<T>> operator->() {
        return std::unique_ptr<value_handle<T>>(new value_handle<T>(_ptr));
    }

    std::unique_ptr<const value_handle<T>> operator->() const {
        return std::unique_ptr<const value_handle<T>>(new value_handle<T>(_ptr));
    }

    T *to_native() {
        void *p; size_t _;
        DrvAPIAddressToNative(_ptr, &p, &_);
        return reinterpret_cast<T*>(p);
    }

    DrvAPI::DrvAPIAddress _ptr;
};

/**
 * Specialization of value_handle for pointer type
 */
template <typename T>
class value_handle<pointer<T>> {
    DRV_API_VALUE_HANDLE_CONSTRUCTORS(pointer<T>)
    DRV_API_VALUE_HANDLE_CAST_OPERATORS_TRIVIAL(pointer<T>)
    DRV_API_VALUE_HANDLE_ASSIGNMENT_OPERATORS_TRIVIAL(pointer<T>)
    
    value_handle & operator=(DrvAPI::DrvAPIAddress v) {
        *this = pointer<T>(v);
        return *this;
    }

    operator DrvAPI::DrvAPIAddress() const {
        pointer<T> p = *this;
        return (DrvAPI::DrvAPIAddress)p;
    }

    value_handle<T> operator[](size_t index) {
        pointer<T> p = *this;
        return p[index];
    }

    const value_handle<T> operator[](size_t index) const {
        pointer<T> p = *this;
        return p[index];
    }

    value_handle<T> operator*() {
        pointer<T> p = *this;
        return *p;
    }

    const value_handle<T> operator*() const {
        pointer<T> p = *this;
        return *p;
    }

    pointer<T> operator->() {
        pointer<T> p = *this;
        return p;
    }

    DRV_API_VALUE_HANDLE_ADDRESSOF_OPERATORS(pointer<T>)
    DRV_API_VALUE_HANDLE_INTERNAL(pointer<T>)
};

template <typename T>
using DrvAPIPointer = DrvAPI::pointer<T>;

}
#endif
