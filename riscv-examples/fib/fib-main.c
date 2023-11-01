// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
// 0 1 2 3 4 5 6  7  8  9 10 11  12  13  14 ...
// 0 1 1 2 3 5 8 13 21 34 55 89 144 233 377 ...
extern int fib(int n);

int main()
{
    int i = 14;
    int r = fib(i);

    char buf[256];
    snprintf(buf, sizeof(buf), "fib(%d) = %d\n", i, r);
    write(STDOUT_FILENO, buf, strlen(buf));
    return r;
}
