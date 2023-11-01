// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include "machine/syscall.h"
        .global _start
_start:
        call    main
done:
        li      a7, SYS_exit
        ecall
        j       done
