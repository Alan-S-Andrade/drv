#include <DrvAPIMain.hpp>
#include <DrvAPIThread.hpp>
#include <DrvAPIMemory.hpp>

int AmoaddMain(int argc, char *argv[])
{
    using namespace DrvAPI;
    DrvAPIAddress signal_addr(8);
    DrvAPIAddress barrier_addr(0);
    int64_t signal = 0xa5a5a5a5a5a5a5a5;
    
    if (DrvAPIThread::current()->threadId() == 0 &&
        DrvAPIThread::current()->coreId() == 0 ) {
        DrvAPI::write<int64_t>(barrier_addr, 0);
        DrvAPI::write<int64_t>(signal_addr, signal);
        printf("Thread %2d: Core %2d: writing signal\n",
               DrvAPIThread::current()->threadId(),
               DrvAPIThread::current()->coreId());
    }

    printf("Thread %2d: Core %2d: waiting for signal\n",
           DrvAPIThread::current()->threadId(),
           DrvAPIThread::current()->coreId());
    
    while (DrvAPI::read<int64_t>(signal_addr) != signal);

    printf("Thread %2d: Core %2d: got signal; adding 1 to barrier\n",
           DrvAPIThread::current()->threadId(),
           DrvAPIThread::current()->coreId());

    int64_t b = DrvAPI::atomic_add<int64_t>(barrier_addr, 1);
    printf("Thread %2d: Core %2d: read %ld after adding to barrier\n",
           DrvAPIThread::current()->threadId(),
           DrvAPIThread::current()->coreId(),
           b);
    
    return 0;
}

declare_drv_api_main(AmoaddMain);
