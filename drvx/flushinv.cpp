#include <DrvAPI.hpp>

using namespace DrvAPI;

int Main(int argc, char *argv[])
{
    pxn_flush_cache(myPXNId());
    pxn_invalidate_cache(myPXNId());
    return 0;
}

declare_drv_api_main(Main);
