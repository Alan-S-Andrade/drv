#ifndef PANDOHAMMER_HARTSLEEP_H
#define PANDOHAMMER_HARTSLEEP_H
#include <pandohammer/register.h>
#include <pandohammer/stringify.h>

static inline void hartsleep(unsigned long cycles)
{
    asm volatile ("csrw " __stringify(MCSR_SLEEP) ", %0" : : "r"(cycles));
}
#endif
