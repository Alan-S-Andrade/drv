#include <string.h>
#include <pandohammer/mmio.h>

#define ARRAY_SIZE(x) \
    (sizeof(x)/sizeof((x)[0]))

//__attribute__((section(".dram")))
char message[] = "Hello, world!\n";

int main()
{
    for (int i = 0; i < strlen(message); i++) {
        ph_print_char(message[i]);
    }
    return 0;
}
