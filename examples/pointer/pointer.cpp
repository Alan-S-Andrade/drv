#include <DrvAPIPointer.hpp>
#include <DrvAPIThread.hpp>
#include <DrvAPIMain.hpp>
#include <stdint.h>
#include <inttypes.h>

#define pr(fmt, ...)                                    \
    do {                                                \
        printf("Core %4d: Thread %4d: " fmt "",         \
               DrvAPIThread::current()->coreId(),       \
               DrvAPIThread::current()->threadId(),     \
               ##__VA_ARGS__);                          \
    } while (0)

struct foo {
    int   baz;
    float bar;
};

int PointerMain(int argc, char* argv[])
{
    using namespace DrvAPI;
    if (DrvAPIThread::current()->threadId() == 0 &&
        DrvAPIThread::current()->coreId() == 0) {
        pr("%s\n", __PRETTY_FUNCTION__);
        DrvAPIPointer<uint64_t> DRAM_BASE(0x80000000ull);
        *DRAM_BASE = 0x55;
        pr(" DRAM_BASE    = 0x%016" PRIx64 "\n", static_cast<uint64_t>(DRAM_BASE));
        pr("&DRAM_BASE[4] = 0x%016" PRIx64 "\n", static_cast<uint64_t>(&DRAM_BASE[4]));
        DrvAPIPointer<foo> fooptr = 0x80000000ull;
        pr(" fooptr       = 0x%016" PRIx64 "\n", static_cast<uint64_t>(fooptr));
        DrvAPIPointer<int> bazptr = DRVAPI_POINTER_MEMBER_POINTER(fooptr,baz);
        DRVAPI_POINTER_DATA_MEMBER(fooptr, baz) = 0xdeadbeef;
        DrvAPIPointer<foo> fooptr2 = &fooptr[4];
        DRVAPI_POINTER_DATA_MEMBER(fooptr2, bar) = 3.14159f;
    }
    return 0;
}

declare_drv_api_main(PointerMain)
