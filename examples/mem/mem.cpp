#include <stdio.h>
#include <DrvAPIMain.hpp>
#include <DrvAPIThread.hpp>
#include <DrvAPIAddress.hpp>
#include <DrvAPIMemory.hpp>

using namespace DrvAPI;

int MemMain(int argc, char *argv[]) {
    printf("Hello from %s\n", __PRETTY_FUNCTION__);

    DrvAPIAddress addr(0);

    uint64_t writeval = 0xdeadbeefcafebabe;
    printf ("writing %lx\n", writeval);
    DrvAPI::write<uint64_t>(addr, writeval);
    uint64_t readback = DrvAPI::read<uint64_t>(addr);
    printf("wrote: %lx, readback: %lx\n", writeval, readback);

    return 0;
}

declare_drv_api_main(MemMain);
