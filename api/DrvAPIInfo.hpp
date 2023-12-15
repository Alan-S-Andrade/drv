// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#ifndef DRV_API_INFO_H
#define DRV_API_INFO_H
#include <DrvAPIThread.hpp>
#include <DrvAPISysConfig.hpp>

namespace DrvAPI
{

////////////////////
// Some constants //
////////////////////
static constexpr int CORE_ID_COMMAND_PROCESSOR = -1;

/////////////////////////////////
// Thread-Relative Information //
/////////////////////////////////
/**
 * return my thread id w.r.t my core
 */
inline int myThreadId() {
    return DrvAPIThread::current()->threadId();
}

/**
 * return my core id w.r.t my pod
 */
inline int myCoreId() {
    return DrvAPIThread::current()->coreId();
}

/**
 * return a core's x  w.r.t my pod
 */
inline int coreXFromId(int core) {
    return core & 7;
}

/**
 * return a core's y  w.r.t my pod
 */
inline int coreYFromId(int core) {
    return (core >> 3) & 7;
}

/**
 * return my core's x  w.r.t my pod
 */
inline int myCoreX() {
    return coreXFromId(myCoreId());
}

/**
 * return my core's y  w.r.t my pod
 */
inline int myCoreY() {
    return coreYFromId(myCoreId());
}

/**
 * return true if I am the command processor
 */
inline bool isCommandProcessor() {
    return myCoreId() == CORE_ID_COMMAND_PROCESSOR;
}

/**
 * return my pod id w.r.t my pxn
 */
inline int myPodId() {
    return DrvAPIThread::current()->podId();
}

/**
 * return my pxn id
 */
inline int myPXNId() {
    return DrvAPIThread::current()->pxnId();
}

inline int myCoreThreads() {
    return DrvAPIThread::current()->coreThreads();
}

//////////////////////
// System Constants //
//////////////////////
inline int numPXNs() {
    return DrvAPISysConfig::Get()->numPXN();
}

inline int numPXNPods() {
    return DrvAPISysConfig::Get()->numPXNPods();
}

inline int numPodCores() {
    return DrvAPISysConfig::Get()->numPodCores();
}

/**
 * size of l1sp in bytes
 */
inline uint64_t coreL1SPSize() {
    return DrvAPISysConfig::Get()->coreL1SPSize();
}

/**
 * size of l2sp in bytes
 */
inline uint64_t podL2SPSize() {
    return DrvAPISysConfig::Get()->podL2SPSize();
}

/**
 * size of pxn's dram in bytes
 */
inline uint64_t pxnDRAMSize() {
    return DrvAPISysConfig::Get()->pxnDRAMSize();
}

//////////
// Time //
//////////
/**
 * return the cycle count
 */
inline uint64_t cycle() {
    return DrvAPIThread::current()->getSystem()->getCycleCount();
}

/**
 * return the hz
 */
inline uint64_t HZ() {
    return DrvAPIThread::current()->getSystem()->getClockHz();
}

inline double seconds() {
    return DrvAPIThread::current()->getSystem()->getSeconds();
}

/**
 * this will force the simulator to do a global statistics dump
 */
inline void outputStatistics() {
    DrvAPIThread::current()->getSystem()->outputStatistics();
}

} // namespace DrvAPI
#endif
