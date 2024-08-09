// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington
#include <pandohammer/mmio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
int main()
{

    char filename[] = "test.txt";
    mode_t mode = 0644;
    int flags = O_WRONLY | O_CREAT | O_TRUNC;

    char buf[128];
    snprintf(buf, sizeof(buf), "open(%s, %x, %d)\n", filename, flags, mode);
    write(STDOUT_FILENO, buf, strlen(buf));
    
    ph_print_int(0);
    snprintf(buf, sizeof(buf), "filenam = %s, %p, %p+%zu=%p\n"
             , filename
             , filename
             , filename
             , strlen(filename)
             , (char*)filename + strlen(filename)
             );
    write(STDOUT_FILENO, buf, strlen(buf));

    int fd = open(filename, flags, mode);
    ph_print_int(1);    
    if (fd == -1) {
        char error[128];
        char errstr[128];
        strerror_r(errno, errstr, sizeof(errstr));
        ph_print_int(2);
        snprintf(error, sizeof(error)-1, "Error opening file: %s: %s\n"                 
                 , filename
                 , errstr);
        ph_print_int(3);
        write(STDOUT_FILENO, error, strlen(error));
        ph_print_int(4);
        return 1;
    }
    
    char message[] = "Hello, world!\n";
    write(fd, message, sizeof(message)-1);
    ph_print_int(5);
    return 0;
}
