#include <DrvAPI.hpp>

using namespace DrvAPI;

int StrideMain(int argc, char *argv[])
{
    int n = atoi(argv[1]);
    int s = atoi(argv[2]);
    long sum = 0;
    printf("n = %d, s = %d\n", n, s);
    pointer<long> v = myAbsoluteDRAMBase();
    for (int i = 0, j = 0; i < n; i++, j+=s) {
        DrvAPI::read<long>(v + j);
    }
    return sum;
}

declare_drv_api_main(StrideMain);
