#include <DrvAPIMain.hpp>
#include <DrvAPIThread.hpp>
#include <DrvAPIAddress.hpp>
#include <DrvAPIMemory.hpp>

using namespace DrvAPI;

int AmoaddMain(int argc, char *argv[])
{
    DrvAPIAddress data_addr   (0);
    DrvAPIAddress signal_addr (8);
    DrvAPIAddress swap_addr  (16);
    uint64_t signal = 0xa5a5a5a5a5a5a5a5;
    if (DrvAPIThread::current()->id() != 0)
        return 0;

    if (DrvAPIThread::current()->coreId() > 1)
        return 0;

    if (DrvAPIThread::current()->coreId() == 0) {
        printf("core %2d: writing %lx to data_addr\n", DrvAPIThread::current()->coreId(), 0xdeadbeefcafebabe);
        DrvAPI::write<uint64_t>(data_addr, 0xdeadbeefcafebabe);
        printf("core %2d: writing %lx to signal_addr\n", DrvAPIThread::current()->coreId(), signal);
        DrvAPI::write<uint64_t>(signal_addr, signal);

        while (DrvAPI::atomic_swap<uint64_t>(swap_addr, 0) != 1)
            printf("core %2d: waiting for swap\n", DrvAPIThread::current()->coreId());
        
    } else {
        while (DrvAPI::read<uint64_t>(signal_addr) != signal)
            printf("core %2d: waiting for signal\n", DrvAPIThread::current()->coreId());
        
        printf("core %2d: read %lx\n", DrvAPIThread::current()->coreId(), DrvAPI::read<uint64_t>(data_addr));
        printf("core %2d: doing the swap\n", DrvAPIThread::current()->coreId());
        DrvAPI::atomic_swap<uint64_t>(swap_addr, 1);
    }
    printf("core %2d: done!\n", DrvAPIThread::current()->coreId());
    return 0;
}

declare_drv_api_main(AmoaddMain);
