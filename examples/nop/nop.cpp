#include <DrvAPI.hpp>

using namespace DrvAPI;

int NopMain(int argc, char *argv[])
{
    int cycles = 1;
    std::string cycles_str = "";
    int arg = 1;
    if (arg < argc) {
        cycles_str = argv[arg++];
        cycles = std::stoi(cycles_str);
    }

    printf("Thread %d on core %d: invoking nop for %d cycles\n",
           DrvAPIThread::current()->threadId(),
           DrvAPIThread::current()->coreId(),
           cycles);

    DrvAPI::nop(cycles);

    printf("Thread %d on core %d: completed nop\n",
           DrvAPIThread::current()->threadId(),
           DrvAPIThread::current()->coreId());    
    return 0;
}

declare_drv_api_main(NopMain);
