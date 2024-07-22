// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington
#include <DrvAPI.hpp>
#include <cstdio>
#include <inttypes.h>
#include <vector>

#define VERBOSE
#ifdef VERBOSE
#define pr_info(fmt, ...)                                               \
    do {                                                                \
        printf("INFO:  PXN %3d: POD: %3d: CORE %3d: " fmt ""            \
               ,myPXNId()                                               \
               ,myPodId()                                               \
               ,myCoreId()                                              \
               ,##__VA_ARGS__);                                         \
    } while (0)
#else
#define pr_info(fmt, ...)                                               \
    do {                                                                \
    } while (0)
#endif

#define pr_error(fmt, ...)                                              \
    do {                                                                \
        printf("ERROR: PXN %3d: POD: %3d: CORE %3d: " fmt ""            \
               ,myPXNId()                                               \
               ,myPodId()                                               \
               ,myCoreId()                                              \
               ,##__VA_ARGS__);                                         \
    } while (0)

int ToNativeMain(int argc, char *argv[])
{
    using namespace DrvAPI;

    std::vector<DrvAPIAddress> test_addresses = {
        myRelativeL1SPBase(),
        myRelativeL1SPBase() + 8,
        myRelativeL1SPBase() + 64,
        myRelativeL1SPBase() + 120,
        myRelativeL1SPBase() + 128,
        myRelativeL1SPBase() + 256,

        myRelativeL2SPBase(),
        myRelativeL2SPBase() + 8,
        myRelativeL2SPBase() + 64,
        myRelativeL2SPBase() + 120,
        myRelativeL2SPBase() + 128,
        myRelativeL2SPBase() + 256,

        myRelativeDRAMBase(),
        myRelativeDRAMBase() + 8,
        myRelativeDRAMBase() + 64,
        myRelativeDRAMBase() + 120,
        myRelativeDRAMBase() + 128,
        myRelativeDRAMBase() + 256,
    };

    for (auto simaddr : test_addresses) {
        DrvAPIAddress addr = simaddr;
        DrvAPIAddressInfo info = decodeAddress(addr);
        pr_info("Translating %s to native pointer\n", info.to_string().c_str());
        void *addr_native = nullptr;
        std::size_t size = 0;
        DrvAPIAddressToNative(addr, &addr_native, &size);
        pr_info("Translated to native pointer %p: size = %zu\n", addr_native, size);

        DrvAPIPointer<uint64_t> as_sim_pointer = addr;
        auto *as_native_pointer = reinterpret_cast<uint64_t*>(addr_native);
        uint64_t wvalue = toAbsoluteAddress(addr);

        uint64_t rvalue = 0;
        pr_info("Writing %010" PRIx64 " to Simulator Address %" PRIx64"\n"
                ,wvalue
                ,(uint64_t)as_sim_pointer
                );

        *as_sim_pointer = wvalue;
        rvalue = *as_native_pointer;
        pr_info("Reading %010" PRIx64 " from Native Address %p\n"
                ,rvalue
                ,as_native_pointer
                );

        if (rvalue != wvalue) {
            pr_error("MISMATCH: Wrote %16" PRIx64 ": Read %16" PRIx64 "\n"
                    ,wvalue
                    ,rvalue
                    );
        }
    }


    return 0;
}

declare_drv_api_main(ToNativeMain);
