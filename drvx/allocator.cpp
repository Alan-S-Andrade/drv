// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include <DrvAPI.hpp>
#include <inttypes.h>
#include <iostream>
using namespace DrvAPI;

struct foo {
    int a_;
    int b_;
    int &a() { return a_; }
    int &b() { return b_; }
    const int &a() const { return a_; }
    const int &b() const { return b_; }
    template <typename Dst, typename Src>
    static void copy(Dst &dst, const Src &src) {
        dst.a() = src.a();
        dst.b() = src.b();
    }    
};

template <>
class DrvAPI::value_handle <foo> {
    DRV_API_VALUE_HANDLE_DEFAULTS(foo);
    DRV_API_VALUE_HANDLE_FIELD(foo, a, int, a_);
    DRV_API_VALUE_HANDLE_FIELD(foo, b, int, b_);
};


DrvAPIGlobalL2SP<int> i;
DrvAPIGlobalL2SP<foo> f;
DrvAPIGlobalL2SP<DrvAPIPointer<int>> pi;

int AllocatorMain(int argc, char *argv[])
{
    using namespace DrvAPI;
    DrvAPIMemoryAllocatorInit();
    for (DrvAPIMemoryType type : {DrvAPIMemoryL1SP, DrvAPIMemoryL2SP, DrvAPIMemoryDRAM}) {
        DrvAPIPointer<int> p0 = DrvAPIMemoryAlloc(type, 0x1000);
        DrvAPIPointer<int> p1 = DrvAPIMemoryAlloc(type, 0x1000);
        std::cout << "Core " << myCoreId() << " Thread " << myThreadId() <<":";
        std::cout << "p0 = " << decodeAddress(p0).to_string() << std::endl;
        std::cout << "Core " << myCoreId() << " Thread " << myThreadId() <<":";
        std::cout << "p1 = " << decodeAddress(p1).to_string() << std::endl;
        std::cout << "Core " << myCoreId() << " Thread " << myThreadId() <<":";
        std::cout << "p0 = 0x" << std::hex << p0 << std::endl;
        std::cout << "Core " << myCoreId() << " Thread " << myThreadId() <<":";
        std::cout << "p1 = 0x" << std::hex << p1 << std::endl;
    }
    DrvAPIPointer<foo> fptr = f.address();
    fptr->a() = 1;
    fptr->b() = 2;
    std::cout << "Core " << myCoreId() << " Thread " << myThreadId() <<":";
    std::cout << "&f = 0x" << std::hex << fptr << std::endl;
    std::cout << "Core " << myCoreId() << " Thread " << myThreadId() <<":";
    std::cout << "f.a = " << fptr->a() << std::endl;
    pi[0] = 1;
    int x = pi[0];
    std::cout << "Core " << myCoreId() << " Thread " << myThreadId() <<":";
    std::cout << "pi[0] = " << x << std::endl;

    return 0;
}
declare_drv_api_main(AllocatorMain);
