// SPDX-License-Identifier: MIT
// Copyright (c) 2024 University of Washington
#include <cstdint>
namespace pandocommand {

struct Place {
    int64_t pxn;
    int64_t pod;
    int64_t core;
    Place(): pxn(0), pod(0), core(0) {}
    Place(int64_t pxn, int64_t pod, int64_t core):
        pxn(pxn), pod(pod), core(core) {}
};

} // namespace pandocommand
