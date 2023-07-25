#pragma once
#include <cstddef>
#include <cstdint>
namespace DrvAPI {

typedef enum  {
    DrvAPIMemAtomicSWAP,
    DrvAPIMemAtomicADD,
} DrvAPIMemAtomicType;

template <typename IntType>
IntType atomic_modify(IntType w, IntType r, DrvAPIMemAtomicType op) {
    switch (op) {
    case DrvAPIMemAtomicSWAP:
        return w;
    case DrvAPIMemAtomicADD:
        return w + r;
    }
    return 0;
}

template <typename IntType>
void atomic_modify(IntType *w, IntType *r, IntType *o, DrvAPIMemAtomicType op) {
    *o = atomic_modify(*w, *r, op);
}

inline void atomic_modify(void *w, void *r, void *o, DrvAPIMemAtomicType op, std::size_t sz) {
    switch (sz) {
    case 1:
        atomic_modify(static_cast<std::uint8_t *>(w), static_cast<std::uint8_t *>(r), static_cast<std::uint8_t *>(o), op);
        break;
    case 2:
        atomic_modify(static_cast<std::uint16_t *>(w), static_cast<std::uint16_t *>(r), static_cast<std::uint16_t *>(o), op);
        break;
    case 4:
        atomic_modify(static_cast<std::uint32_t *>(w), static_cast<std::uint32_t *>(r), static_cast<std::uint32_t *>(o), op);
        break;
    case 8:
        atomic_modify(static_cast<std::uint64_t *>(w), static_cast<std::uint64_t *>(r), static_cast<std::uint64_t *>(o), op);
        break;
    }
}

}
