#ifndef PANDOHAMMER_HARTSLEEP_H
#define PANDOHAMMER_HARTSLEEP_H
#include <pandohammer/cpuinfo.h>
#define MCSR_SLEEP 0x7A5
static inline void hartsleep(unsigned long cycles)
{
    asm volatile ("csrw " __stringify(MCSR_SLEEP) ", %0" : : "r"(cycles));
}
#endif
