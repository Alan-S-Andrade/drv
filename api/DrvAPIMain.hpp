#pragma once

typedef void (*drv_api_main_t)(int argc, char *argv[]);

/**
 * @brief Declare the main function for an application.
 */
#define declare_drv_api_main(main_function)                     \
    extern "C" int __drv_api_main(int argc, char *argv[])       \
    {                                                           \
        return main_function(argc, argv);                       \
    }
