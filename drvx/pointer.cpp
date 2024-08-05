// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include <DrvAPI.hpp>
#include <stdint.h>
#include <inttypes.h>

#define pr(fmt, ...)                                    \
    do {                                                \
        printf("Core %4d: Thread %4d: " fmt "",         \
               DrvAPIThread::current()->coreId(),       \
               DrvAPIThread::current()->threadId(),     \
               ##__VA_ARGS__);                          \
    } while (0)

#define FIELD(type, name)                               \
    public: type name##_;                               \
public: const type& name() const { return name##_; }    \
public: type &name() { return name##_; }

struct foo {
    FIELD(int, baz);
    FIELD(float, bar);
    template <typename Dst, typename Src>
    static void copy(Dst &dst, const Src &src) {
        dst.baz() = src.baz();
        dst.bar() = src.bar();
    }
};

struct bar {
    FIELD(int, obaz);
    FIELD(float, obar);
    float   sum() const { return obaz() + obar(); }

    template <typename Dst, typename Src>
    static void copy(Dst &dst, const Src &src) {
        dst.obaz() = src.obaz();
        dst.obar() = src.obar();
    }
};

using namespace DrvAPI;

template <>
class DrvAPI::value_handle<bar> {
public:
    DRV_API_VALUE_HANDLE_DEFAULTS(bar);
    DRV_API_VALUE_HANDLE_FIELD(bar, obaz, int, obar_);
    DRV_API_VALUE_HANDLE_FIELD(bar, obar, float, obaz_);
    float sum() const { return obaz() + obar(); }
};

template <>
class DrvAPI::value_handle<foo> {
public:
    DRV_API_VALUE_HANDLE_DEFAULTS(foo);
    DRV_API_VALUE_HANDLE_FIELD(foo, baz, int, bar_);
    DRV_API_VALUE_HANDLE_FIELD(foo, bar, float, baz_);
};

int PointerMain(int argc, char* argv[])
{
    using namespace DrvAPI;
    if (DrvAPIThread::current()->threadId() == 0 &&
        DrvAPIThread::current()->coreId() == 0) {
        pr("%s\n", __PRETTY_FUNCTION__);
        DrvAPIPointer<uint64_t> DRAM_BASE = myRelativeDRAMBase();
        *DRAM_BASE = 0x55;
        pr(" DRAM_BASE    = 0x%016" PRIx64 "\n", static_cast<uint64_t>(DRAM_BASE));
        pr("&DRAM_BASE[4] = 0x%016" PRIx64 "\n", static_cast<uint64_t>(&DRAM_BASE[4]));
        pr(" DRAM_BASE[0] = 0x%016" PRIx64 "\n", static_cast<uint64_t>(DRAM_BASE[0]));
        // foo_ref fptr = foo_ref(0x80000000ull);
        // fptr.baz() = 7;
        // fptr.bar() = 3.14159f;
        // pr("fptr.baz() = %d\n", static_cast<int>(fptr.baz()));
        // pr("fptr.bar() = %f\n", static_cast<float>(fptr.bar()));
        DrvAPIPointer<bar> bptr(0x80000000ull);
        DrvAPIPointer<bar> bref = bptr[0].address();
        bref->obaz() = 7;
        bref->obar() = 3.14159f;
        pr("bref.obaz() = %d\n", static_cast<int>(bref->obaz()));
        pr("bref.obar() = %f\n", static_cast<float>(bref->obar()));
        pr("bref.sum()  = %f\n", bref->sum());
        // void pointer
        DrvAPIPointer<void> voidptr = myRelativeL2SPBase();
        pr("voidptr = 0x%016" PRIx64 "\n", static_cast<uint64_t>(voidptr));
    }
    return 0;
}

declare_drv_api_main(PointerMain);
