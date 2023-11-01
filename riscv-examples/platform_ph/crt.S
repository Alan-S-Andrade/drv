// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include "machine/syscall.h"
        .global _start
_start:
        # Initialize global pointer
        .option push
        .option norelax
        la      gp, __global_pointer$
        .option pop
        call    main
done:
        li      a7, SYS_exit
        ecall
        j       done
