#include <DrvAPI.hpp>
#include <stdio.h>

int Main(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++)
        printf("%s\n", argv[i]);
    return 0;
}

declare_drv_api_main(Main);

