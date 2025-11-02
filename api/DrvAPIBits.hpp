#ifndef __DRV_API_BITS_HPP__
#define __DRV_API_BITS_HPP__

namespace DrvAPI
{
namespace bits {
/**
 * get the minimum number of bits required to represent a value
 */
static inline size_t bitlength(int v) {
    if (v < 0) {
        v = -v;
    }
    size_t l = 0;
    while (v) {
        v >>= 1;
        l++;
    }
    return l;
}

/**
 * settable/gettable handle to a bit range
 */
template <typename UINT, unsigned HI, unsigned LO, unsigned TAG=0>
struct bitrange_handle {
public:
    typedef UINT uint_type;
    static constexpr unsigned HI_BIT = HI;
    static constexpr unsigned LO_BIT = LO;

    bitrange_handle(UINT &i) : i(i) {}
    ~bitrange_handle() = default;
    bitrange_handle(bitrange_handle &&o) = default;
    bitrange_handle &operator=(bitrange_handle &&o) = default;
    bitrange_handle(const bitrange_handle &o) = default;
    bitrange_handle &operator=(const bitrange_handle &o) = default;

    static constexpr UINT lo() {
        return LO;
    }
    static constexpr UINT hi() {
        return HI;
    }
    static constexpr UINT bits() {
        return HI - LO + 1;
    }

    static constexpr UINT mask()
    {
        return ((1ull << (HI - LO + 1)) - 1) << LO;
    }

    static UINT getbits(UINT in)
    {
        return (in & mask()) >> LO;
    }

    static void setbits(UINT &in, UINT val)
    {
        in &= ~mask();
        in |=  mask() & (val << LO);
    }

    operator UINT() const { return getbits(i); }

    bitrange_handle &operator=(UINT val) {
        setbits(i, val);
        return *this;
    }

    UINT &i;
};

/**
 * class for getting and setting bits in a dynamic range
 */
template <typename UINT>
struct dynamic_bitfield {
public:
    typedef UINT uint_type;

    dynamic_bitfield() : lo_(0), hi_(0) {}

    dynamic_bitfield(UINT hi, UINT lo) : lo_(lo), hi_(hi) {}

    UINT lo() const {
        return lo_;
    }
    UINT hi() const {
        return hi_;
    }
    UINT bits() const {
        return hi() - lo() + 1;
    }
    UINT mask() const {
        return ((1ull << (hi() - lo() + 1)) - 1) << lo();
    }
    UINT getbits(UINT i) const {
        return (i & mask()) >> lo();
    }
    void setbits(UINT &i, UINT val) const{
        i &= ~mask();
        i |=  mask() & (val << lo());
    }

    UINT operator()(UINT i) const {
        return getbits(i);
    }

    UINT lo_;
    UINT hi_;
};

/**
 * settable/gettable handle to a dynamic bit range
 */
template <typename UINT>
struct dynamic_bitrange_handle {
public:
    typedef UINT uint_type;

    dynamic_bitrange_handle(UINT &i) : i(i) {}
    dynamic_bitrange_handle(UINT &i, const dynamic_bitfield<UINT> bits) : i(i), bits_(bits) {}
    dynamic_bitrange_handle(UINT &i, UINT hi, UINT lo) : i(i), bits_(hi, lo) {}
    ~dynamic_bitrange_handle() = default;
    dynamic_bitrange_handle(dynamic_bitrange_handle &&o) = default;
    dynamic_bitrange_handle &operator=(dynamic_bitrange_handle &&o) = default;
    dynamic_bitrange_handle(const dynamic_bitrange_handle &o) = default;
    dynamic_bitrange_handle &operator=(const dynamic_bitrange_handle &o) = default;

    UINT lo() const {
        return bits_.lo();
    }
    UINT hi() const {
        return bits_.hi();
    }
    UINT bits() const {
        return bits_.bits();
    }

    UINT mask() const {
        return bits_.mask();
    }

    UINT getbits() {
        return bits_.getbits(i);
    }

    void setbits(UINT val) {
        bits_.setbits(i, val);
    }

    operator UINT() const {
        return getbits();
    }

    dynamic_bitrange_handle &operator=(UINT val) {
        setbits(val);
        return *this;
    }

    UINT &i;
    dynamic_bitfield<UINT> bits_;
};
}
}
#endif
