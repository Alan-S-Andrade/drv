#include <DrvAPI.hpp>
#include <cstdio>

int SimpleMain(int argc, char *argv[]) {
    printf("my pxn: %2d/%2d, "
           "my pod: %2d/%2d, "
           "my core: %2d/%2d, "
           "my thread: %2d/%2d \n"
           ,DrvAPI::myPXNId(), DrvAPI::numPXNs()
           ,DrvAPI::myPodId(), DrvAPI::numPXNPods()
           ,DrvAPI::myCoreId(),DrvAPI::numPodCores()
           ,DrvAPI::myThreadId(), DrvAPI::myCoreThreads()
           );
    return 0;
}

declare_drv_api_main(SimpleMain);
