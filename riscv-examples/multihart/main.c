#include <string.h>
#include <stdint.h>

/**
 * print_int.c
 */
static inline void print_int(long x)
{
    *(volatile long*)0xFFFFFFFFFFFF0000 = x;
}

static inline void print_hex(unsigned long x)
{
    *(volatile unsigned long*)0xFFFFFFFFFFFF0008 = x;
}

static inline void print_char(char x)
{
    *(volatile char*)0xFFFFFFFFFFFF0010 = x;
}

#define ARRAY_SIZE(x) \
    (sizeof(x)/sizeof((x)[0]))


static inline int hartid() {
    int hart;
    asm volatile ("csrr %0, mhartid" : "=r" (hart));
    return hart;
}

static inline int64_t amoswap(int64_t w, int64_t *p) {
    int64_t r;
    asm volatile ("amoswap.d %0, %1, 0(%2)"
                  : "=r" (r)
                  : "r" (w), "r" (p)
                  : "memory");
    return r;
}

static inline int64_t amoadd(int64_t w, int64_t *p) {
    int64_t r;
    asm volatile ("amoadd.d %0, %1, 0(%2)"
                  : "=r" (r)
                  : "r" (w), "r" (p)
                  : "memory");
    return r;
}

int64_t x = -1;
int64_t y =  0;

int main() {
    int64_t id  = hartid();
    //print_int(id);
    // swap id with x
    //print_int(amoswap(id, &x));
    print_int(amoadd(1, &y));
    return 0;
}
