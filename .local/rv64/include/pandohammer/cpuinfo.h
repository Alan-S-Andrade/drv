// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#ifndef PANDOHAMMER_CPUINFO_H
#define PANDOHAMMER_CPUINFO_H
#include <stdint.h>
#include <pandohammer/stringify.h>
#include <pandohammer/register.h>
#ifdef __cplusplus
extern "C" {
#endif

/**
 * thread id wrt my core
 */ 
static inline int myThreadId()
{
    int64_t tid;
    asm volatile ("csrr %0, mhartid" : "=r"(tid));
    return (int)tid;
}

/**
 * core id wrt my pod
 */
static inline int myCoreId()
{
    int64_t cid;
    asm volatile ("csrr %0, " __stringify(MCSR_MCOREID) : "=r"(cid));
    return (int)cid;
}

/**
 * pod id wrt my pxn
 */
static inline int myPodId()
{
    int64_t pid;
    asm volatile ("csrr %0, " __stringify(MCSR_MPODID) : "=r"(pid));
    return (int)pid;
}

/**
 * pxn id
 */
static inline int myPXNId()
{
    int64_t xid;
    asm volatile ("csrr %0, " __stringify(MCSR_MPXNID) : "=r"(xid));
    return (int)xid;
}

/**
 * number of hardware threads on my core
 */
static inline int myCoreThreads()
{
    int64_t harts;
    asm volatile ("csrr %0, " __stringify(MCSR_MCOREHARTS) : "=r"(harts));
    return (int)harts;
}

/**
 * number of pxns in system
 */
static inline int numPXN()
{
    int64_t num;
    asm volatile ("csrr %0, " __stringify(MCSR_MNUMPXN) : "=r"(num));
    return (int)num;
}

/**
 * number of core columns in a pod
 */
static inline int numPodCoresX()
{
    int64_t cores;
    asm volatile ("csrr %0, " __stringify(MCSR_MPODCORESX) : "=r"(cores));
    return (int)cores;
}

/**
 * number of core rows in a pod
 */
static inline int numPodCoresY()
{
    int64_t cores;
    asm volatile ("csrr %0, " __stringify(MCSR_MPODCORESY) : "=r"(cores));
    return (int)cores;
}

/**
 * number of cores in a pod
 */
static inline int numPodCores()
{
    return numPodCoresX() * numPodCoresY();
}


/**
 * get the id from the x and y coordinates
 */
static inline int coreIdFromXY(int x, int y)
{
    return x + y*numPodCoresX();
}

/**
 * get the x coordinate from the core id
 */
static inline int coreXFromId(int id)
{
    return id % numPodCoresX();
}

/**
 * get the y coordinate from the core id
 */
static inline int coreYFromId(int id)
{
    return id / numPodCoresX();
}

/**
 * get the x coordinate from the core id
 */
static inline int myCoreX()
{
   return coreXFromId(myCoreId());
}

/**
 * get the y coordinate from the core id
 */
static inline int myCoreY()
{
    return coreYFromId(myCoreId());
}

/**
 * number of pods in a pxn
 */
static inline int numPXNPods()
{
    int64_t pods;
    asm volatile ("csrr %0, " __stringify(MCSR_MPXNPODS) : "=r"(pods));
    return (int)pods;
}

/**
 * size of l1sp in bytes
 */
static inline uint64_t coreL1SPSize() {
    uint64_t l1sp_size;
    asm volatile ("csrr %0, " __stringify(MCSR_MCOREL1SPSIZE) : "=r"(l1sp_size));
    return l1sp_size;
}

/**
 * size of l2sp in bytes
 */
static inline uint64_t podL2SPSize() {
    uint64_t l2sp_size;
    asm volatile ("csrr %0, " __stringify(MCSR_MPODL2SPSIZE) : "=r"(l2sp_size));
    return l2sp_size;
}

/**
 * size of pxn's dram in bytes
 */
static inline uint64_t pxnDRAMSize() {
    uint64_t dram_size;
    asm volatile ("csrr %0, " __stringify(MCSR_MPXNDRAMSIZE) : "=r"(dram_size));
    return dram_size;
}

/**
 * get the current cycle count
 */
static inline uint64_t cycle() {
    uint64_t cycle;
    asm volatile ("rdcycle %0" : "=r"(cycle));
    return cycle;
}

#ifdef __cplusplus
}
#endif
#endif
