#include <DrvAPIMain.hpp>
#include <DrvAPIThread.hpp>
#include <cstdio>

int SimpleMain(int argc, char *argv[]) {
    printf("Simple hello world\n");
    return 0;
}

declare_drv_api_main(SimpleMain);
