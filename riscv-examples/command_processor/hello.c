// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include <pandohammer/mmio.h>
#include <pandohammer/staticdecl.h>
#include <stdint.h>

__dram__ uint64_t signal = 0;

int main(int argc, char *argv[])
{
    while (signal == 0);

    ph_print_char('H');
    ph_print_char('i');
    ph_print_char('\n');

    return 0;
}
