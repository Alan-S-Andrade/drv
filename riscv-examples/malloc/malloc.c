#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
int *p, *q;
void malloc_test()
{
    char buf[256];
    p = malloc(10 * sizeof(int));
    snprintf(buf, sizeof(buf), "malloc(10) = %x\n", p);
    write(STDOUT_FILENO, buf, strlen(buf));
    q = malloc(20 * sizeof(int));
    snprintf(buf, sizeof(buf), "malloc(20) = %x\n", q);
    write(STDOUT_FILENO, buf, strlen(buf));
    free(p);
    p = malloc(30 * sizeof(int));
    snprintf(buf, sizeof(buf), "malloc(30) = %x\n", p);
    write(STDOUT_FILENO, buf, strlen(buf));
}
