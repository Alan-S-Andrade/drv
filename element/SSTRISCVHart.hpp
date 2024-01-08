// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include <RISCVHart.hpp>
namespace SST {
namespace Drv {

class RISCVSimHart : public RISCVHart {
public:

    virtual ~RISCVSimHart() {}

    int & ready() { return _ready; }
    int   ready() const { return _ready; }

    int & exit() { return _exit; }
    int   exit() const { return _exit; }

    int64_t & exitCode() { return _exit_code; }
    int64_t   exitCode() const { return _exit_code; }
    
    int _ready = true;
    int _exit = false;
    int64_t _exit_code = 0;
};

}
}
