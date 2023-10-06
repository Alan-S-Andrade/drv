#ifndef PANDOHAMMER_MMIO_H
#define PANDOHAMMER_MMIO_H


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


#endif
