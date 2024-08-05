// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington
#include <DrvAPI.hpp>
#include <cstdio>
#include <inttypes.h>
#include <vector>

#define pr_info(fmt, ...)                                               \
    do {                                                                \
        printf("PXN %3d: POD: %3d: CORE %3d: " fmt ""                   \
               ,myPXNId()                                               \
               ,myPodId()                                               \
               ,myCoreId()                                              \
               ,##__VA_ARGS__);                                         \
    } while (0)

#define FIELD(type, name)                       \
    type name##_;                               \
    type &name() { return name##_; }            \
    const type &name() const { return name##_; }

struct id_type {
    FIELD(int64_t, pxn);
    FIELD(int64_t, pod);
    FIELD(int64_t, core);
    FIELD(int64_t, thread);
    template <typename Dst, typename Src>
    static void copy(Dst &dst, const Src &src) {
        dst.pxn() = src.pxn();
        dst.pod() = src.pod();
        dst.core() = src.core();
        dst.thread() = src.thread();
    }
};

template <>
class DrvAPI::value_handle<id_type> {
public:
    DRV_API_VALUE_HANDLE_DEFAULTS(id_type);
    DRV_API_VALUE_HANDLE_FIELD(id_type, pxn, int64_t, pxn_);
    DRV_API_VALUE_HANDLE_FIELD(id_type, pod, int64_t, pod_);
    DRV_API_VALUE_HANDLE_FIELD(id_type, core, int64_t, core_);
    DRV_API_VALUE_HANDLE_FIELD(id_type, thread, int64_t, thread_);
};
    
int ToAddressMain(int argc, char *argv[])
{
    using namespace DrvAPI;
    struct id_type id;
    DrvAPIAddress addr = 0;
    std::size_t size = 0;
    DrvAPINativeToAddress(&id, &addr, &size);

    DrvAPIPointer<id_type> id_ptr = addr;

    id_ptr->pxn() = myPXNId();
    id_ptr->pod() = myPodId();
    id_ptr->core() = myCoreId();
    id_ptr->thread() = myThreadId();

    id_type *native = nullptr;
    size_t _;
    DrvAPIAddressToNative(id_ptr, (void**)&native, &_);
    if (native != &id) {
        pr_info("FAIL: AddressToNative(NativeToAddress(&id)) != &id\n");
    } else if (id.pxn() != (int64_t) myPXNId() ||
               id.pod() != (int64_t) myPodId() ||
               id.core() != (int64_t) myCoreId() ||
               id.thread() != (int64_t) myThreadId()) {
        pr_info("FAIL: id fields don't match mine\n");
    } else {
        pr_info("PASS: all checks succeeded \n");
    }

    return 0;
}

declare_drv_api_main(ToAddressMain);
