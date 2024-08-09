// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <pandohammer/atomic.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/mmio.h>
#include <pandohammer/address.h>
#include <pandohammer/staticdecl.h>



__l1sp__ long l1sp_var;
__l2sp__ long l2sp_var;
__dram__ long dram_var;

int main()
{
    long stack;
    uintptr_t addr = (uintptr_t)&stack;
    ph_print_hex(addr);
    ph_print_int(ph_address_is_absolute(addr));
    if (ph_address_is_absolute(addr) && ph_address_absolute_is_l1sp(addr))
    {
        ph_print_int(ph_address_absolute_core(addr));
        ph_print_hex(ph_address_absolute_l1sp_offset(addr));
    } else {
        addr = ph_address_relative_l1sp_to_absolute(addr, 1, 1, 1);
        ph_print_hex(addr);
    }
    return 0;
}
