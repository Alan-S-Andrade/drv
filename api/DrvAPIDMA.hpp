#ifndef DRV_API_DMA_HPP
#define DRV_API_DMA_HPP
#include <DrvAPIAddress.hpp>
namespace DrvAPI
{

class DrvAPIDMAJob
{
public:
    /**
     * @brief Construct a new DrvAPIDMAJob object
     */
    DrvAPIDMAJob(char *native, DrvAPIAddress sim, size_t size)
        : native(native)
        , sim(sim)
        , size(size) {
    }

    /**
     * @brief Empty constructor
     */
    DrvAPIDMAJob(): DrvAPIDMAJob(nullptr, 0, 0) {}

    char      *native; //!< pointer to native memory
    DrvAPIAddress sim; //!< pointer to simulation memory
    size_t       size; //!< size of memory
};

/**
 * @brief DMA job to copy from native to simulation memory
 *
 */
class DrvAPIDMANativeToSim : public DrvAPIDMAJob
{
public:
    DrvAPIDMANativeToSim(char *native, DrvAPIAddress sim, size_t size)
        : DrvAPIDMAJob(native, sim, size) {
    }
    DrvAPIAddress dst() const { return sim; }
    const char *src() const { return native; }
};

/**
 * @brief DMA job to copy from simulation to native memory
 *
 */
class DrvAPIDMASimToNative : public DrvAPIDMAJob
{
public:
    DrvAPIDMASimToNative(char *native, DrvAPIAddress sim, size_t size)
        : DrvAPIDMAJob(native, sim, size) {
    }
    DrvAPIAddress src() const { return sim; }
    char *dst() const { return native; }
};


/**
 * @brief DMA data from simulator memory to native memory
 */
void dmaSimToNative(const DrvAPIDMASimToNative *jobs, size_t count);

/**
 * @brief DMA data from native memory to simulator memory
 */
void dmaNativeToSim(const DrvAPIDMANativeToSim *jobs, size_t count);

}
#endif
