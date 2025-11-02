#include <DrvAPIDMA.hpp>
#include <DrvAPIMemory.hpp>
#include <DrvAPIAddressToNative.hpp>
#include <DrvAPIInfo.hpp>
#include <cstring>

namespace DrvAPI
{

static
void dmaSimToNative(const DrvAPIDMASimToNative &job)
{
    char *dst = job.dst();
    DrvAPIAddress src = job.src();
    size_t sz = job.size;
    while (sz > 0) {
        size_t chunk = 0;
        void *src_as_native = nullptr;
        DrvAPIAddressToNative(src, &src_as_native, &chunk);
        chunk = std::min(chunk, sz);
        memcpy(dst, src_as_native, chunk);
        sz  -= chunk;
        dst += chunk;
        src += chunk;
    }
}

void dmaSimToNative(const DrvAPIDMASimToNative *jobs, size_t count)
{
    // flush the cache
    pxn_flush_cache(myPXNId());

    // handle each job
    for (size_t i = 0; i < count; i++) {
        const DrvAPIDMASimToNative &job = jobs[i];
        dmaSimToNative(job);
    }
}

static
void dmaNativeToSim(const DrvAPIDMANativeToSim &job)
{
    DrvAPIAddress dst = job.dst();
    const char *src = job.src();
    size_t sz = job.size;
    while (sz > 0) {
        size_t chunk = 0;
        void *dst_as_native = nullptr;
        DrvAPIAddressToNative(dst, &dst_as_native, &chunk);
        chunk = std::min(chunk, sz);
        memcpy(dst_as_native, src, chunk);
        sz  -= chunk;
        src += chunk;
        dst += chunk;
    }
}

void dmaNativeToSim(const DrvAPIDMANativeToSim *jobs, size_t count)
{
    // flush and invalidate the cache
    pxn_flush_cache(myPXNId());
    pxn_invalidate_cache(myPXNId());

    // handle each job
    for (size_t i = 0; i < count; i++){
        const DrvAPIDMANativeToSim &job = jobs[i];
        dmaNativeToSim(job);
    }
}

}
