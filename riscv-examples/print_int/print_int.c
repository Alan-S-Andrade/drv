// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

/**
 * print_int.c
 */
static inline void print_int(long x)
{
    *(long*)0xFFFFFFFFFFFF0000 = x;
}

static inline void print_hex(unsigned long x)
{
    *(unsigned long*)0xFFFFFFFFFFFF0008 = x;
}

static inline void print_char(char x)
{
    *(char*)0xFFFFFFFFFFFF0010 = x;
}

int main()
{
    print_int(1234567890);
    print_hex(0x1234567890ABCDEF);
    print_char('A');    
    return 0;
}
