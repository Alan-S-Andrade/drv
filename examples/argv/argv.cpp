#include <DrvAPIMain.hpp>
#include <cstdio>

int ArgvMain(int argc, char *argv[])
{
    printf("Hello from %s\n", __PRETTY_FUNCTION__);
    for (int i = 0; i < argc; i++) {
        printf("argv[%d] = %s\n", i, argv[i]);
    }
    return 0;
}

declare_drv_api_main(ArgvMain);
