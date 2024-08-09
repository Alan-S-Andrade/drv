#include <pandohammer/mmio.h>
#include <pandohammer/staticdecl.h>
#include <stdint.h>

__dram__ int64_t command_processor_present = 0;

int main(int argc, char *argv[])
{
    ph_print_hex((unsigned long)&command_processor_present);
    ph_print_int(command_processor_present);
    return 0;
}
