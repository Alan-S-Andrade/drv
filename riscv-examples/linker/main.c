#include <math.h>
#include <string.h>
#include <pandohammer/mmio.h>

#define ARRAY_SIZE(x) \
    (sizeof(x)/sizeof((x)[0]))

char message[] = "Hello, world!\n";

int main()
{
    ph_print_hex(&message);
    return 0;
}
