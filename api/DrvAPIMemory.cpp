#include <DrvAPIMemory.hpp>
#include <DrvAPIThread.hpp>
#include <DrvAPIInfo.hpp>
#include <DrvAPIAddressMap.hpp>
namespace DrvAPI
{

template <typename Op>
void pxn_op_on_cache(int pxn, Op op)
{
    if (pxnDRAMHasCache()) {
        DrvAPIAddress interleave = pxnDRAMAddressInterleave();
        DrvAPIAddress cache_addr = absolutePXNDRAMBase(pxn);
        for (int b = 0; b < numPXNDRAMCacheBanks(); b++) {
            for (int l = 0; l < numPXNDRAMCacheLines(); l++) {
                op(cache_addr, l);
            }
            cache_addr += interleave;
        }
    }
    return;    
}

/**
 * @brief flush and invalidate dram cache on a pxn
 */
void pxn_flush_cache(int pxn)
{
    pxn_op_on_cache(pxn, [](DrvAPIAddress cache_addr, int l) {
        flush_cache(cache_addr, l);
    });
    return;
}

/**
 * @brief flush and invalidate dram cache on a pxn
 */
void pxn_invalidate_cache(int pxn)
{
    pxn_op_on_cache(pxn, [](DrvAPIAddress cache_addr, int l) {
        invalidate_cache(cache_addr, l);
    });
    return;
}

} // namespace DrvAPI
