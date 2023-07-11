#include <DrvAPIMain.hpp>
#include <DrvAPIThread.hpp>

int ThreadMain(int argc, char *argv[]) {
    printf("Hello from thread %p\n", static_cast<void*>(DrvAPI::DrvAPIThread::current()));
    return 0;
}

declare_drv_api_main(ThreadMain);
