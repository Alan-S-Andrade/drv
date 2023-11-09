// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include "DrvAPIAllocator.hpp"
#include "DrvAPIGlobal.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <atomic>
#include <inttypes.h>
namespace DrvAPI
{

DrvAPIGlobalL2SP<uint64_t> drvapi_global_memory[DrvAPIMemoryType::DrvAPIMemoryNTypes];

static std::atomic<int> initialized;

void DrvAPIMemoryAllocatorInit() {
    // only initialize once
    if (initialized.exchange(1) == 1) {
        return;
    }

    // init global memory
    for (int type = 0; type < DrvAPIMemoryType::DrvAPIMemoryNTypes; ++type) {
        // rebase this to make sure is offset from the start of the section
        drvapi_global_memory[type].rebase();
        std::stringstream ss;
        ss << "DrvAPIMemoryAllocatorInit: &drvapi_global_memory[" << type << "] = "
           << std::hex << &drvapi_global_memory[type];

        std::cout << ss.str() << std::endl;

        DrvAPISection &section = DrvAPISection::GetSection(static_cast<DrvAPIMemoryType>(type));
        uint64_t sz = (section.getSize() + 7) & ~7;
        drvapi_global_memory[type] = section.getBase() + sz;
    }
}

DrvAPIPointer<void> DrvAPIMemoryAlloc(DrvAPIMemoryType type, size_t size) {
    // size should be 8-byte aligned
    size = (size + 7) & ~7;
    uint64_t addr = DrvAPI::atomic_add<uint64_t>(&drvapi_global_memory[type], size);
    return DrvAPIPointer<void>(addr);
}

void DrvAPIMemoryFree(const DrvAPIPointer<void> &ptr) {
}


}
