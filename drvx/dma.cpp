#include <DrvAPI.hpp>
#include <vector>
#include <inttypes.h>

int DMAMain(int argc, char *argv[])
{
    using namespace DrvAPI;
    std::vector<DrvAPIAddress> test_addresses = {
#define L1SP
#define DRAM
#ifdef  L1SP
        myRelativeL1SPBase(),
        myRelativeL1SPBase() + 8,
        myRelativeL1SPBase() + 64,
        myRelativeL1SPBase() + 120,
        myRelativeL1SPBase() + 128,
        myRelativeL1SPBase() + 256,
#endif
#ifdef  L2SP
        myRelativeL2SPBase(),
        myRelativeL2SPBase() + 8,
        myRelativeL2SPBase() + 64,
        myRelativeL2SPBase() + 120,
        myRelativeL2SPBase() + 128,
        myRelativeL2SPBase() + 256,
#endif
#ifdef  DRAM
        myRelativeDRAMBase(),
        myRelativeDRAMBase() + 8,
        myRelativeDRAMBase() + 64,
        myRelativeDRAMBase() + 120,
        myRelativeDRAMBase() + 128,
        myRelativeDRAMBase() + 256,
#endif
    };

    // test dma to simulation
    for (DrvAPIAddress addr : test_addresses)
    {
        DrvAPIAddress data = addr;
        DrvAPIDMANativeToSim j((char*)&data, addr, sizeof(data));
        printf("DMA (native => simulation) writing %" PRIx64 " from native %p to simulation %" PRIx64 "\n"
               , data
               , &data
               , addr
               );
        dmaNativeToSim(&j, 1);
        // read it back
        DrvAPIAddress rback = read<DrvAPIAddress>(addr);
        if (rback != data)
        {
            printf("DMA to simulation failed at address %p\n", addr);
            return 1;
        }
    }

    // test dma from simulation
    for (DrvAPIAddress addr : test_addresses)
    {
        DrvAPIAddress data = addr ^ -1;
        DrvAPIDMASimToNative j((char*)&data, addr, sizeof(data));
        printf("DMA (simulation => native) writing %" PRIx64 " from simulation %" PRIx64 " to native %p\n"
               , data
               , addr
               , &data
               );
        dmaSimToNative(&j, 1);
        // read it back
        DrvAPIAddress rback = read<DrvAPIAddress>(addr);
        if (rback != data)
        {
            printf("DMA to native failed at address %p\n", addr);
            return 1;
        }
    }
    return 0;
}

declare_drv_api_main(DMAMain);
