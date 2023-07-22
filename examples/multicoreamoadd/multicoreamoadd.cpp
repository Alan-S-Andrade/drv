#include "DrvAPIAddress.hpp"
#include "DrvAPIThread.hpp"
#include "DrvAPIMain.hpp"
#include "DrvAPIMemory.hpp"

using namespace DrvAPI;

int AmoaddMain(int argc, char *argv[])
{
    DrvAPIAddress addr(0);
    
    if (DrvAPIThread::current()->id() != 0)
        return 0;

    printf("core %2d: adding 1\n", DrvAPIThread::current()->coreId());
    int r = 0;    
    r = DrvAPI::atomic_add<uint64_t>(addr, 1);
    printf("core %2d: read %2d after amoadd\n", DrvAPIThread::current()->coreId(), r);
    
    while ((r = DrvAPI::read<uint64_t>(addr)) < 2)
        printf("core %2d: waiting for all cores: (%2d)\n", DrvAPIThread::current()->coreId(), r);
    
    return 0;
}

declare_drv_api_main(AmoaddMain);
