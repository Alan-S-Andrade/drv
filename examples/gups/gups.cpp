#include <DrvAPIMain.hpp>
#include <DrvAPIMemory.hpp>
#include <DrvAPIThread.hpp>
#include <string>
#include <cstdint>
#include <inttypes.h>

using namespace DrvAPI;

DrvAPIAddress DRAM_START = 0x80000000;
DrvAPIAddress TABLE = DRAM_START;

int GupsMain(int argc, char *argv[])
{
    std::string tbl_size_str = "67108864";
    std::string thread_n_updates_str = "1024";
    if (argc > 1) {
        tbl_size_str = argv[1];
    }
    if (argc > 2) {
        thread_n_updates_str = argv[2];
    }
    int64_t tbl_size = std::stoll(tbl_size_str);
    int64_t thread_n_updates = std::stoll(thread_n_updates_str);

    printf("Core %4d: Thread %4d: tble_size = %" PRId64 ", thread_n_updates = %" PRId64 "\n",           
           DrvAPIThread::current()->coreId(),
           DrvAPIThread::current()->threadId(),
           tbl_size,
           thread_n_updates);
    
    for (int64_t u = 0; u < thread_n_updates; u++) {
        int64_t i = rand() % tbl_size;
        int64_t addr = DRAM_START + i * sizeof(int64_t);
        auto  val = read<int64_t>(addr);
        write(addr, val ^ addr);
    }
    printf("Core %4d: Thread %4d: done\n",
           DrvAPIThread::current()->coreId(),
           DrvAPIThread::current()->threadId());
    return 0;    
}

declare_drv_api_main(GupsMain);
