// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington
#include <string>
#include <sstream>
#include <iomanip>
#include <inttypes.h>
#include "DrvAPIAddressMap.hpp"
#include "DrvAPIThread.hpp"
namespace DrvAPI {

/**
 * Returns the relative address of the local core's L1 scratchpad
 */
DrvAPIAddress myRelativeL1SPBase() {
    DrvAPIThread *thread = DrvAPIThread::current();
    return thread->getDecoder().this_cores_relative_l1sp_base();
}

/**
 * Returns the relative address of the local core's L2 scratchpad
 */
DrvAPIAddress myRelativeL2SPBase() {
    DrvAPIThread *thread = DrvAPIThread::current();
    return thread->getDecoder().this_pods_relative_l2sp_base();
}

/**
 * Returns the relative address of the local core's DRAM
 */
DrvAPIAddress myRelativeDRAMBase() {
    DrvAPIThread *thread = DrvAPIThread::current();
    return thread->getDecoder().this_pxns_relative_dram_base();
}

/**
 * Returns the relative address of the end of the local core's DRAM
 */
DrvAPIAddress myRelativeDRAMEnd() {
    DrvAPIThread *thread = DrvAPIThread::current();
    DrvAPIAddress base = thread->getDecoder().this_pxns_relative_dram_base();
    using bitfield = DrvAPIAddressDecoder::bitfield;
    bitfield is_dram_bit = thread->getDecoder().relative_is_dram_;
    return base + (1ull << is_dram_bit.hi()) - 1;
}

/**
 * Returns the absolute address of the local core's L1 scratchpad
 */
DrvAPIAddress myAbsoluteL1SPBase() {
    DrvAPIThread *thread = DrvAPIThread::current();
    return thread->getDecoder().this_cores_absolute_l1sp_base();
}

/**
 * Returns the absolute address of the local core's L2 scratchpad
 */
DrvAPIAddress myAbsoluteL2SPBase() {
    DrvAPIThread *thread = DrvAPIThread::current();
    return thread->getDecoder().this_pods_absolute_l2sp_base();
}

/**
 * Returns the absolute address of the local core's DRAM
 */
DrvAPIAddress myAbsoluteDRAMBase() {
    DrvAPIThread *thread = DrvAPIThread::current();
    return thread->getDecoder().this_pxns_absolute_dram_base();
}

/**
 * Returns decoded information about the given address
 */
DrvAPIAddressInfo decodeAddress(DrvAPIAddress addr) {
    DrvAPIThread *thread = DrvAPIThread::current();
    DrvAPIAddressInfo info = thread->getDecoder().decode(addr);
    return info;
}

/**
 * Encode the address info into an address
 */
DrvAPIAddress encodeAddressInfo(const DrvAPIAddressInfo &info) {
    DrvAPIThread *thread = DrvAPIThread::current();
    return thread->getDecoder().encode(info);
}

/**
 * Converts an address that may be relative to an absolute address
 */
DrvAPIAddress toAbsoluteAddress(DrvAPIAddress addr) {
    DrvAPIThread *thread = DrvAPIThread::current();
    return thread->getDecoder().to_absolute(addr);
}

/**
 * Converts an address that may be absolute to a relative address
 */
DrvAPIAddress toRelativeAddress(DrvAPIAddress addr) {
    DrvAPIThread *thread = DrvAPIThread::current();
    DrvAPIAddressInfo info = thread->getDecoder().decode(addr);
    info.set_absolute(false);
    return thread->getDecoder().encode(info);
}

/**
 * Returns the absolute address of a core's control register
 */
DrvAPIAddress absoluteCoreCtrlBase(int64_t pxn, int64_t pod, int64_t core) {
    DrvAPIThread *thread = DrvAPIThread::current();
    DrvAPIAddressInfo info;
    info.set_absolute(true)
        .set_core_ctrl()
        .set_pxn(pxn)
        .set_pod(pod)
        .set_core(core)
        .set_offset(0);
    return thread->getDecoder().encode(info);
}

/**
 * Returns the absolute address of a pxn's dram
 */
DrvAPIAddress absolutePXNDRAMBase(int64_t pxn) {
    DrvAPIThread *thread = DrvAPIThread::current();
    DrvAPIAddressInfo info;
    info.set_absolute(true)
        .set_dram()
        .set_pxn(pxn)
        .set_offset(0);
    return thread->getDecoder().encode(info);
}


} // namespace DrvAPI

