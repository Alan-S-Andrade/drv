// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington
#include <DrvAPI.hpp>
#include <cstdio>
#include <inttypes.h>

#define pr_info(fmt, ...)                                               \
    do {                                                                \
        printf("PXN %3d: POD: %3d: CORE %3d: " fmt ""                   \
               ,myPXNId()                                               \
               ,myPodId()                                               \
               ,myCoreId()                                              \
               ,##__VA_ARGS__);                                         \
    } while (0)

using namespace DrvAPI;

DrvAPIGlobalL1SP<int> g_l1sp;
DrvAPIGlobalL2SP<int> g_l2sp;
DrvAPIGlobalDRAM<int> g_dram;

int GlobalsMain(int argc, char *argv[])
{
    pr_info("&g_l1sp     = %016" PRIx64 " %s\n"
            ,(DrvAPIAddress)&g_l1sp
            ,decodeAddress(&g_l1sp).to_string().c_str());
    pr_info("&g_l2sp     = %016" PRIx64 " %s\n"
            , (DrvAPIAddress)&g_l2sp
            , decodeAddress(&g_l2sp).to_string().c_str());
    pr_info("&g_dram     = %016" PRIx64 " %s\n"
            , (DrvAPIAddress)&g_dram
            , decodeAddress(&g_dram).to_string().c_str());

    pr_info("toAbsolute(&g_l1sp) = %" PRIx64 " %s\n"
            , toAbsoluteAddress(&g_l1sp)
            , decodeAddress(toAbsoluteAddress(&g_l1sp)).to_string().c_str());
    pr_info("toAbsolute(&g_l2sp) = %" PRIx64 " %s\n"
            , toAbsoluteAddress(&g_l2sp)
            , decodeAddress(toAbsoluteAddress(&g_l2sp)).to_string().c_str());
    pr_info("toAbsolute(&g_dram) = %" PRIx64 " %s\n"
            , toAbsoluteAddress(&g_dram)
            , decodeAddress(toAbsoluteAddress(&g_dram)).to_string().c_str());

    if (!decodeAddress(&g_l1sp).is_l1sp()) {
        pr_info("ERROR: g_l1sp is not in L1\n");
        return 1;
    }
    if (!decodeAddress(&g_l2sp).is_l2sp()) {
        pr_info("g_l2sp is not in L2\n");
        return 1;
    }
    if (!decodeAddress(&g_dram).is_dram()) {
        pr_info("g_dram is not in main memory\n");
        return 1;
    }
    return 0;
}

declare_drv_api_main(GlobalsMain);
