#include "DrvAPIFunction.hpp"

using namespace DrvAPI;

__attribute__((constructor))
static void InitializeDrvAPIFunctionTypeInfoV()
{
    for (DrvAPIFunctionTypeInfo *p = &__start_drv_api_function_typev;
         p < &__stop_drv_api_function_typev;
         ++p) {
        p->id = (p-&__start_drv_api_function_typev);
    }
}
