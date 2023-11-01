// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#ifndef PANDOHAMMER_CPUINFO_H
#define PANDOHAMMER_CPUINFO_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MCSR_MCOREID    0xF15
#define MCSR_MPODID     0xF16
#define MCSR_MPXNID     0xF17
#define MCSR_MCOREHARTS 0xF18
#define MCSR_MPODCORES  0xF19
#define MCSR_MPXNPODS   0xF1A
#define MCSR_MNUMPXN    0xF1B

#ifndef __stringify
#define __stringify_1(x) #x
#define __stringify(x) __stringify_1(x)
#endif

/**
 * thread id wrt my core
 */ 
inline int myThreadId()
{
    int64_t tid;
    asm volatile ("csrr %0, mhartid" : "=r"(tid));
    return (int)tid;
}

/**
 * core id wrt my pod
 */
inline int myCoreId()
{
    int64_t cid;
    asm volatile ("csrr %0, " __stringify(MCSR_MCOREID) : "=r"(cid));
    return (int)cid;
}

/**
 * pod id wrt my pxn
 */
inline int myPodId()
{
    int64_t pid;
    asm volatile ("csrr %0, " __stringify(MCSR_MPODID) : "=r"(pid));
    return (int)pid;
}

/**
 * pxn id
 */
inline int myPXNId()
{
    int64_t xid;
    asm volatile ("csrr %0, " __stringify(MCSR_MPXNID) : "=r"(xid));
    return (int)xid;
}

/**
 * number of hardware threads on my core
 */
inline int myCoreThreads()
{
    int64_t harts;
    asm volatile ("csrr %0, " __stringify(MCSR_MCOREHARTS) : "=r"(harts));
    return (int)harts;
}

/**
 * number of pxns in system
 */
inline int numPXN()
{
    int64_t num;
    asm volatile ("csrr %0, " __stringify(MCSR_MNUMPXN) : "=r"(num));
    return (int)num;
}

/**
 * number of cores in a pod
 */
inline int numPodCores()
{
    int64_t cores;
    asm volatile ("csrr %0, " __stringify(MCSR_MPODCORES) : "=r"(cores));
    return (int)cores;
}

/**
 * number of pods in a pxn
 */
inline int numPXNPods()
{
    int64_t pods;
    asm volatile ("csrr %0, " __stringify(MCSR_MPXNPODS) : "=r"(pods));
    return (int)pods;
}
#ifdef __cplusplus
}
#endif
#endif
