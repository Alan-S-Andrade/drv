#ifndef DRV_API_CORE_XY_H
#define DRV_API_CORE_XY_H
namespace DrvAPI
{

/**
 * return a core's x  w.r.t my pod
 */
inline int coreXFromId(int core) {
    return core & 7;
}

/**
 * return a core's y  w.r.t my pod
 */
inline int coreYFromId(int core) {
    return (core >> 3) & 7;
}

/**
 * return a core's id from its x y
 */
inline int coreIdFromXY(int x, int y) {
    return x + (y << 3);
}

}
#endif
