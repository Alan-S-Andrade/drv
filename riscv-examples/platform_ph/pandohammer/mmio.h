// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#ifndef PANDOHAMMER_MMIO_H
#define PANDOHAMMER_MMIO_H
#ifdef __cplusplus
extern "C" {
#endif

static inline void ph_print_int(long x)
{
    *(volatile long*)0xFFFFFFFFFFFF0000 = x;
}

static inline void ph_print_hex(unsigned long x)
{
    *(volatile unsigned long*)0xFFFFFFFFFFFF0008 = x;
}

static inline void ph_print_char(char x)
{
    *(volatile char*)0xFFFFFFFFFFFF0010 = x;
}


#ifdef __cplusplus
}
#endif
#endif
