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


int ToNativeMain(int argc, char *argv[])
{
    using namespace DrvAPI;

    std::vector<DrvAPIAddress> test_addresses = {
        DrvAPIVAddress::MyL1Base().encode(),
        DrvAPIVAddress::MyL1Base().encode() + 0x100,
        DrvAPIVAddress::MyL2Base().encode() + 0x1000,
    };

    for (auto simaddr : test_addresses) {
        DrvAPIVAddress addr = simaddr;
        pr_info("Translating %s to native pointer\n", addr.to_string().c_str());
        void *addr_native = nullptr;
        std::size_t size = 0;
        DrvAPIAddressToNative(addr.encode(), &addr_native, &size);
        pr_info("Translated to native pointer %p: size = %zu\n", addr_native, size);

        DrvAPIPointer<uint64_t> as_sim_pointer = addr.encode();
        auto *as_native_pointer = reinterpret_cast<uint64_t*>(addr_native);
        uint64_t value = addr
            .to_physical(myPXNId(), myPodId(), myCoreId() >> 3, myCoreId() & 0x7)
            .encode();

        pr_info("Writing %010" PRIx64 " to Simulator Address %" PRIx64"\n"
                ,value
                ,(uint64_t)as_sim_pointer
                );

        *as_sim_pointer = value;
        pr_info("Reading %010" PRIx64 " from Native Address %p\n"
                ,*as_native_pointer
                ,as_native_pointer
                );
    }


    return 0;
}

declare_drv_api_main(ToNativeMain);
