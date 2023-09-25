#include <unistd.h>
int main(int argc, char *argv[])
{
    write(STDOUT_FILENO, "Hello, world!\n", 14);
    return 0;
}
