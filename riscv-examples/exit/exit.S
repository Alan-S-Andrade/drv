#include <machine/syscall.h>
        .global _start
_start:
        li      a7, SYS_exit
        ecall   
done:
        j       done
