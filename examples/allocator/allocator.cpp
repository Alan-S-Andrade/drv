#include <DrvAPI.hpp>
#include <inttypes.h>
#include <iostream>
using namespace DrvAPI;

struct foo {
    int a;
    int b;
};
DRV_API_REF_CLASS_BEGIN(foo)
DRV_API_REF_CLASS_DATA_MEMBER(foo, a)
DRV_API_REF_CLASS_DATA_MEMBER(foo, b)
DRV_API_REF_CLASS_END(foo)

DrvAPIGlobalDRAM<int> i;
DrvAPIGlobalDRAM<foo> f;

int AllocatorMain(int argc, char *argv[])
{
    using namespace DrvAPI;
    if (DrvAPIThread::current()->threadId() == 0 &&
        DrvAPIThread::current()->coreId() == 0) {
        DrvAPIMemoryAllocatorInit();
        DrvAPIPointer<void> p0 = DrvAPIMemoryAlloc(DrvAPIMemoryDRAM, 0x1000);
        DrvAPIPointer<void> p1 = DrvAPIMemoryAlloc(DrvAPIMemoryDRAM, 0x1000);        
        foo_ref fref = &f;
        std::cout << "p0 = 0x" << std::hex << p0 << std::endl;
        std::cout << "p1 = 0x" << std::hex << p1 << std::endl;        
        fref.a() = 1;
        fref.b() = 2;
        std::cout << "&f = 0x" << std::hex << &fref << std::endl;
        std::cout << "f.a = " << fref.a() << std::endl;
    }    
    return 0;
}
declare_drv_api_main(AllocatorMain);
