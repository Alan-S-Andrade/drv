// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#ifndef PANDOHAMMER_ATOMIC_H
#define PANDOHAMMER_ATOMIC_H
#include <stdint.h>
inline int atomic_fetch_add(volatile int32_t *ptr, int32_t val)
{
    int32_t ret;
    asm volatile("amoadd.w %0, %2, 0(%1)" : "=r"(ret): "r"(ptr) , "r"(val));
    return ret;
}

inline int64_t atomic_fetch_add(volatile int64_t *ptr, int64_t val)
{
    int64_t ret;
    asm volatile("amoadd.d %0, %2, 0(%1)" : "=r"(ret): "r"(ptr) , "r"(val));
    return ret;
}
inline int32_t atomic_swap(volatile int32_t *ptr, int32_t val)
{
    int32_t ret;
    asm volatile("amoswap.w %0, %2, 0(%1)" : "=r"(ret): "r"(ptr) , "r"(val));
    return ret;
}

inline int64_t atomic_swap(volatile int64_t *ptr, int64_t val)
{
    int64_t ret;
    asm volatile("amoswap.d %0, %2, 0(%1)" : "=r"(ret): "r"(ptr) , "r"(val));
    return ret;
}
#endif
