// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington
/* Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved. */

#ifndef PANDOHAMMER_MMIO_H
#define PANDOHAMMER_MMIO_H
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

static inline void ph_print_float(float x)
{
    *(volatile float*)0xFFFFFFFFFFFF0000 = x;
}

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

static inline void ph_puts(char *cstr)
{
    for (long i = 0; i < strlen(cstr); i++) {
        ph_print_char(cstr[i]);
    }
}

static inline void ph_print_time(long tag)
{
    *(volatile char*)0xFFFFFFFFFFFF0018 = tag;
}

static inline void ph_stat_phase(long phase)
{
    *(volatile long*)0xFFFFFFFFFFFF0020 = phase;
}

struct ph_bulk_load_desc {
    long filename_addr;
    long dest_addr;
    long size;
    long result;
};

static inline long ph_bulk_load_file(struct ph_bulk_load_desc *desc)
{
    *(volatile long*)0xFFFFFFFFFFFF0028 = (long)desc;
    return desc->result;
}

struct ph_pgas_sit_desc {
    long index;       /* SIT slot (0..3) */
    long vsid;        /* Segment ID value to match */
    long vsid_bits;   /* Number of bits for VSID field */
    long vgid_hi;     /* High bit of VGID in address */
    long vgid_lo;     /* Low bit of VGID in address */
};

struct ph_pgas_ptt_desc {
    long sit_index;    /* Which SIT slot */
    long vgid;         /* Virtual Group ID */
    long target_pxn;   /* Physical target PXN */
    long offset_base;  /* Base DRAM offset added before encoding (0 = no shift) */
};

static inline void ph_pgas_sit_write(struct ph_pgas_sit_desc *d)
{
    *(volatile long*)0xFFFFFFFFFFFF0030 = (long)d;
}

static inline void ph_pgas_ptt_write(struct ph_pgas_ptt_desc *d)
{
    *(volatile long*)0xFFFFFFFFFFFF0038 = (long)d;
}

#ifdef __cplusplus
}
#endif
#endif
