// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington
#include "DrvAPIThreadState.hpp"
#include "DrvAPIAddressMap.hpp"
#include "DrvAPIInfo.hpp"
namespace DrvAPI {

DrvAPIMem::DrvAPIMem(DrvAPIAddress address)
    : can_resume_(false)
    , address_(0) {
    DrvAPIThread *thread = DrvAPIThread::current();
    const DrvAPIAddressDecoder &decoder = thread->getDecoder();
    address_ = decoder.to_absolute(address);
}

}
