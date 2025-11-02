#ifndef DRV_API_CORE_XY_H
#define DRV_API_CORE_XY_H
#include <DrvAPIBits.hpp>
#include <DrvAPISysConfig.hpp>

namespace DrvAPI
{

/**
 * return a core's x  w.r.t my pod
 */
inline int coreXFromId(int core) {
    return core % DrvAPISysConfig::Get()->numPodCoresX();
}

/**
 * return a core's y  w.r.t my pod
 */
inline int coreYFromId(int core) {
    return core / DrvAPISysConfig::Get()->numPodCoresX();
}


/**
 * return a core's id from its x y
 */
inline int coreIdFromXY(int x, int y) {
    return x + y * DrvAPISysConfig::Get()->numPodCoresX();
}

}
#endif
