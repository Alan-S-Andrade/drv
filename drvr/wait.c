#include <pandohammer/hartsleep.h>

#include <stdio.h>

void sleep(long cycles) {
#ifdef USE_SLEEP
    hartsleep(cycles);
#else
    for (long i = 0; i < cycles; i++);
#endif
}

int main() {
    sleep(10000);
    return 0;
}