#ifndef ICACHE_HPP
#define ICACHE_HPP
#include <cmath>
#include <sstream>
#include "ICacheBacking.hpp"

namespace interp
{
inline std::string fmt_addr(uint64_t addr) {
    std::stringstream ss;
    ss << "0x" << std::hex << std::setw(16) << std::setfill('0') << addr;
    return ss.str();
}

inline std::string fmt_bool(bool b) {
    return b ? "true" : "false";
}

template <typename INT>
inline INT clog2(INT x) {
    return std::ceil(std::log2(x));
}
}

class ICache
{
public:
    /**
     * bitrange_handle - A class to handle bitfields in a register.
     */
    template <typename INT>
    struct bitrange_handle {
    public:
        typedef INT  value_type;
        typedef INT& reference_type;

        /**
         * constructor
         */
        bitrange_handle(value_type &val, value_type hi, value_type lo)
            :v_(val)
            ,hi_(hi)
            ,lo_(lo) {
        }

        /**
         * destructor
         */
        ~bitrange_handle() = default;

        /**
         * move constructor
         */
        bitrange_handle(bitrange_handle &&o) = default;

        /**
         * move constructor
         */
        bitrange_handle(const bitrange_handle &o) = default;

        /**
         * move assignment operator
         */
        bitrange_handle &operator=(bitrange_handle &&o) {
            setbits(o.getbits());
            return *this;
        }

        /**
         * copy assignment operator
         */
        bitrange_handle &operator=(const bitrange_handle &o) {
            setbits(o.getbits());
            return *this;
        }


        /**
         * Conversion operator to an integer
         */
        operator value_type() const {
            return getbits();
        }

        /**
         * Assignment operator to an integer
         */
        bitrange_handle &operator=(value_type val) {
            setbits(val);
            return *this;
        }

        value_type lo()   const {
            return lo_;
        }

        value_type hi() const {
            return hi_;
        }

        const reference_type v() const {
            return v_;
        }

        reference_type v() {
            return v_;
        }

        value_type bits() const {
            return hi_ - lo_ + 1;
        }

        value_type mask() const {
            return ((1ull << bits()) - 1) << lo();
        }

        value_type getbits() const
        {
            return (v() & mask()) >> lo();
        }

        void setbits(value_type val)
        {
            v() &= ~mask();
            v() |=  mask() & (val << lo());
        }

        reference_type v_;
        value_type    hi_;
        value_type    lo_;
    };


    /**
     * bitrange - A class to make bitrange_handle
     */
    template <typename INT>
    struct  bitrange {
        typedef INT value_type;
        bitrange(value_type hi, value_type lo) : hi_(hi), lo_(lo) {}
        bitrange() : hi_(0), lo_(0) {}

        bitrange_handle<value_type>
        operator()(value_type &v) const {
            return bitrange_handle<value_type>(v, hi_, lo_);
        }

        value_type hi() const {
            return hi_;
        }
        value_type lo() const {
            return lo_;
        }
        value_type hi_;
        value_type lo_;
    };

    /**
     * Set - A class to represent a set in the cache.
     */
    class Set {
    public:
        Set(size_t associativity) : associativity_(associativity) {}

        size_t search(Elf64_Addr tag) {
            for (size_t w = 0; w < ways_.size(); w++) {
                if (ways_[w] == tag) {
                    return w;
                }
            }
            return -1u;
        }

        bool find(Elf64_Addr tag) {
            //std::cout << __PRETTY_FUNCTION__ << " finding : " << fmt_addr(tag) << std::endl;
            size_t w = search(tag);
            if (w == -1u) {
                return false;
            }
            //std::cout << __PRETTY_FUNCTION__ << " found   : " << fmt_addr(tag) << ", way #" << w << std::endl;
            // move the way to the front
            auto way = ways_[w];
            ways_.erase(ways_.begin() + w);
            ways_.insert(ways_.begin(), way);
            //std::cout << __PRETTY_FUNCTION__ << " updated : " << fmt_addr(tag) << std::endl;
            return true;
        }

        void fetch(Elf64_Addr tag) {
            if (ways_.size() == associativity_) {
                ways_.pop_back();
            }
            ways_.insert(ways_.begin(), tag);
        }

    private:
        std::vector<Elf64_Addr> ways_;
        size_t associativity_;
    };

    /**
     * constructor
     */
    ICache(ICacheBacking* icache_backing, size_t instructions, size_t associativity)
        : icache_backing_(icache_backing)
        , instructions_(instructions)
        , associativity_(associativity) {
        auto idx_bits = interp::clog2(instructions / associativity);
        index_ = bitrange<Elf64_Addr>(idx_bits+02-1, 02);
        tag_   = bitrange<Elf64_Addr>(63,   idx_bits+02);
        cache_ = std::vector<Set>(sets(), Set(associativity));
    }

    size_t sets() {
        return instructions_ / associativity_;
    }

    /**
     * return (hit, data)
     */
    std::pair<bool, uint32_t> read(Elf64_Addr addr) {
        //std::cout << __PRETTY_FUNCTION__ << " reading: " << fmt_addr(addr) << std::endl;
        bool found = find(addr);
        //std::cout << __PRETTY_FUNCTION__ << "    find:" << fmt_addr(addr) << ", " << fmt_bool(found) << std::endl;
        if (!found) {
            fetch(addr);
        }
        //std::cout << __PRETTY_FUNCTION__ << "    read: " << fmt_addr(addr) << std::endl;
        return {found, icache_backing_->read(addr)};
    }

    ICacheBacking* backing() {
        return icache_backing_;
    }

    bool find(Elf64_Addr addr) {
        Elf64_Addr index = index_(addr);
        Elf64_Addr tag = tag_(addr);
        auto & set = cache_[index];
        return set.find(tag);
    }

    void fetch(Elf64_Addr addr) {
        Elf64_Addr index = index_(addr);
        Elf64_Addr tag = tag_(addr);
        auto & set = cache_[index];
        set.fetch(tag);
    }

private:
    ICacheBacking* icache_backing_;
    bitrange<Elf64_Addr> index_;
    bitrange<Elf64_Addr> tag_;
    size_t associativity_;
    size_t instructions_;
    std::vector<Set> cache_;
};
#endif
