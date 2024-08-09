// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/staticdecl.h>
#include <pandohammer/address.h>
#include <stdint.h>


extern void *_edata;
static  int thread_updates = (int)THREAD_UPDATES;
static  uint64_t table_size = (uint64_t)TABLE_SIZE;

static  uint64_t random(uint64_t *seed)
{
    uint64_t x = *seed;
    x ^=  x << 13;
    x ^= ~x >> 7;
    x ^=  x << 17;
    *seed = x;
    return x;
}

int main()
{
    uint64_t seed
        = myThreadId()
        + myCoreThreads() * myCoreId()
        + myCoreThreads() * numPodCores() * myPodId()
        + myCoreThreads() * numPodCores() * numPXNPods() * myPXNId();

    uintptr_t table_addr = 0;
    table_addr = ph_address_set_absolute(table_addr, 1);
    table_addr = ph_address_absolute_set_dram(table_addr, 1);
    table_addr = ph_address_absolute_set_pxn(table_addr, 0);
    table_addr = ph_address_absolute_set_dram_offset(table_addr, 0);

    uint64_t *table = (uint64_t*)table_addr;

    for (int u = 0; u < thread_updates; u++) {
        uint64_t index = random(&seed) % table_size;
        uint64_t *addr = &table[index];
        //ph_print_hex((unsigned long)addr);
        uint64_t value = *addr;
        value ^= (uint64_t)addr;
        *addr = value;
    }
    return 0;
}
