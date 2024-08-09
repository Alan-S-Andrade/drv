#include "vread.hpp"
#include <cstdio>
#include "pandohammer/atomic.h"
#include "pandohammer/cpuinfo.h"
#include "pandohammer/mmio.h"
#include "pandohammer/staticdecl.h"
#include "pandohammer/address.h"

#ifndef CORES
#define CORES (numPodCores())
#endif

#ifndef THREADS
#define THREADS (myCoreThreads()*numPodCores())
#endif

#ifndef BLOCK_SIZE
#define BLOCK_SIZE 8
#endif

#define ID (myCoreId()*myCoreThreads()+myThreadId())

__l2sp__ idx_type sync = 0;

int main(int argc, char *argv[])
{
    uintptr_t dram_base_addr = 0;
    dram_base_addr = ph_address_set_absolute(dram_base_addr, 1);
    dram_base_addr = ph_address_absolute_set_dram(dram_base_addr, 1);
    dram_base_addr = ph_address_absolute_set_pxn(dram_base_addr, 0);
    dram_base_addr = ph_address_absolute_set_dram_offset(dram_base_addr, 0);

    int64_t* a = (int64_t*)dram_base_addr;

    for (idx_type i = BLOCK_SIZE*ID; i < vsize; i+=BLOCK_SIZE*THREADS) {
        for (idx_type j = 0; j < BLOCK_SIZE; j++) {
            atomic_load_i64(&a[i+j]);
        }
    }

    atomic_fetch_add_i64(&sync, 1);
    while (atomic_load_i64(&sync) != THREADS) {
        // spin
    }
    return 0;
}
