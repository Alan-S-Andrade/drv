#ifndef PANDOHAMMER_ADDRESS_H
#define PANDOHAMMER_ADDRESS_H
#include <stdint.h>
#include "address_map.h"

/**
 * @brief Make a mask from a range of bits
 * 
 * @param hi The high bit of the range
 * @param lo The low bit of the range
 * @return uintptr_t The mask
 */
#define PH_MAKE_MASK(hi,lo) (((1ul << ((hi) - (lo) + 1)) - 1) << (lo))

/**
 * @brief Make a mask from a range of bits and shift it to the right
 * 
 * @param hi The high bit of the range
 * @param lo The low bit of the range
 * @return uintptr_t The mask, shifted to the right
 */
#define PH_MAKE_MASK_SHIFT(hi,lo) (PH_MAKE_MASK(hi,lo) >> (lo))


/**
 * @brief Mask to check if address is absolute
 */
#define IS_ABSOLUTE_MASK PH_MAKE_MASK(IS_ABSOLUTE_HI, IS_ABSOLUTE_LO)

/**
 * @brief Mask to check if the absolute address is DRAM
 */
#define ABSOLUTE_IS_DRAM_MASK PH_MAKE_MASK(ABSOLUTE_IS_DRAM_HI, ABSOLUTE_IS_DRAM_LO)

/**
 * @brief Mask to check if the absolute address is L2SP
 */
#define ABSOLUTE_IS_L2SP_MASK PH_MAKE_MASK(ABSOLUTE_IS_L2SP_HI, ABSOLUTE_IS_L2SP_LO)

/**
 * @brief Mask to check if the absolute address is control registers
 */
#define ABSOLUTE_IS_CTRL_MASK PH_MAKE_MASK(ABSOLUTE_IS_CTRL_HI, ABSOLUTE_IS_CTRL_LO)

/**
 * @brief Mask to check the pxn of the address
 */
#define ABSOLUTE_PXN_MASK PH_MAKE_MASK(ABSOLUTE_PXN_HI, ABSOLUTE_PXN_LO)

/**
 * @brief Mask to check the pod of the address
 */
#define ABSOLUTE_POD_MASK PH_MAKE_MASK(ABSOLUTE_POD_HI, ABSOLUTE_POD_LO)

/**
 * @brief Mask to check the core of the address
 */
#define ABSOLUTE_CORE_MASK PH_MAKE_MASK(ABSOLUTE_CORE_HI, ABSOLUTE_CORE_LO)

/**
 * @brief Mask to get dram offset of the address
 */
#define ABSOLUTE_DRAM_OFFSET_MASK PH_MAKE_MASK(ABSOLUTE_DRAM_OFFSET_HI, ABSOLUTE_DRAM_OFFSET_LO)

/**
 * @brief Mask to get l2sp offset of the address
 */
#define ABSOLUTE_L2SP_OFFSET_MASK PH_MAKE_MASK(ABSOLUTE_L2SP_OFFSET_HI, ABSOLUTE_L2SP_OFFSET_LO)

/**
 * @brief Mask to get l1sp offset of the address
 */
#define ABSOLUTE_L1SP_OFFSET_MASK PH_MAKE_MASK(ABSOLUTE_L1SP_OFFSET_HI, ABSOLUTE_L1SP_OFFSET_LO)

/**
 * @brief Mask to get control offset of the address
 */
#define ABSOLUTE_CTRL_OFFSET_MASK PH_MAKE_MASK(ABSOLUTE_CTRL_OFFSET_HI, ABSOLUTE_CTRL_OFFSET_LO)


/**
 * @brief Get the bits under a mask
 * 
 * @param value The value to extract bits from
 * @param mask The mask to apply to the value
 * @return uintptr_t The bits under the mask
 */
static inline uintptr_t ph_get_bits_under_mask(uintptr_t value, uintptr_t mask)
{
    return value & mask;
}

/**
 * @brief Get the bits under a mask and shift them to the right
 * 
 * @param value The value to extract bits from
 * @param mask The mask to apply to the value
 * @param shift The number of bits to shift the result to the right
 * @return uintptr_t The bits under the mask, shifted to the right
 */
static inline uintptr_t ph_get_bits_under_mask_shift(uintptr_t value, uintptr_t mask, uintptr_t shift)
{
    return ph_get_bits_under_mask(value, mask) >> shift;
}

/**
 * @brief Set the bits under a mask
 * 
 * @param value The value to set bits in
 * @param mask The mask to apply to the value
 * @param bits The bits to set
 * @return uintptr_t The value with the bits set under the mask
 */
static inline uintptr_t ph_set_bits_under_mask(uintptr_t value, uintptr_t mask, uintptr_t bits)
{
    return (value & ~mask) | (bits & mask);
}

/**
 * @brief Set the bits under a mask and shift them to the left
 * 
 * @param value The value to set bits in
 * @param mask The mask to apply to the value
 * @param bits The bits to set
 * @param shift The number of bits to shift the bits to the left
 * @return uintptr_t The value with the bits set under the mask, shifted to the left
 */
static inline uintptr_t ph_set_bits_under_mask_shift(uintptr_t value, uintptr_t mask, uintptr_t bits, uintptr_t shift)
{
    return ph_set_bits_under_mask(value, mask, bits << shift);
}

/**
 * @brief Check if an address is absolute
 * 
 * @param address The address to check
 * @return uintptr_t nonzero if the address is absolute, 0 otherwise
 */
static inline uintptr_t ph_address_is_absolute(uintptr_t address)
{
    return ph_get_bits_under_mask(address, IS_ABSOLUTE_MASK);
}

/**
 * @brief Set the absolute bit of an address
 */
static inline uintptr_t ph_address_set_absolute(uintptr_t address, uintptr_t is_abs)
{
    return ph_set_bits_under_mask_shift(address, IS_ABSOLUTE_MASK, is_abs, IS_ABSOLUTE_LO);
}

/**
 * @brief Get the absolute address from a relative address
 * 
 * @param address The relative address
 * @return uintptr_t The absolute address
 */
static inline uintptr_t ph_address_is_relative(uintptr_t address)
{
    return !ph_address_is_absolute(address);
}

/**
 * @brief Check if an absolute address is DRAM
 * 
 * @param address The absolute address to check
 * @return uintptr_t nonzero if the address is DRAM, 0 otherwise
 */
static inline uintptr_t ph_address_absolute_is_dram(uintptr_t address)
{
    return ph_get_bits_under_mask(address, ABSOLUTE_IS_DRAM_MASK);
}

/**
 * @brief Set the DRAM bit of an absolute address
 */
static inline uintptr_t ph_address_absolute_set_dram(uintptr_t address, uintptr_t is_dram)
{
    return ph_set_bits_under_mask_shift(address, ABSOLUTE_IS_DRAM_MASK, is_dram, ABSOLUTE_IS_DRAM_LO);
}

/**
 * @brief Check if an absolute address is L2SP
 * 
 * @param address The absolute address to check
 * @return uintptr_t nonzero if the address is L2SP, 0 otherwise
 */
static inline uintptr_t ph_address_absolute_is_l2sp(uintptr_t address)
{
    return ph_get_bits_under_mask(address, ABSOLUTE_IS_L2SP_MASK);
}

/**
 * @brief Set the L2SP bit of an absolute address
 */
static inline uintptr_t ph_address_absolute_set_l2sp(uintptr_t address, uintptr_t is_l2sp)
{
    return ph_set_bits_under_mask_shift(address, ABSOLUTE_IS_L2SP_MASK, is_l2sp, ABSOLUTE_IS_L2SP_LO);
}

/**
 * @brief Check if an absolute address is control registers
 * 
 * @param address The absolute address to check
 * @return uintptr_t nonzero if the address is control registers, 0 otherwise
 */
static inline uintptr_t ph_address_absolute_is_ctrl(uintptr_t address)
{
    return ph_get_bits_under_mask(address, ABSOLUTE_IS_CTRL_MASK);
}

/**
 * @brief Set the control bit of an absolute address
 */
static inline uintptr_t ph_address_absolute_set_ctrl(uintptr_t address, uintptr_t is_ctrl)
{
    return ph_set_bits_under_mask_shift(address, ABSOLUTE_IS_CTRL_MASK, is_ctrl, ABSOLUTE_IS_CTRL_LO);
}

/**
 * @brief Check if an absolute address is L1SP
 * 
 * @param address The absolute address
 * @return uintptr_t The PXN of the address
 */
static inline uintptr_t ph_address_absolute_is_l1sp(uintptr_t address)
{
    return !ph_address_absolute_is_dram(address)
        && !ph_address_absolute_is_l2sp(address)
        && !ph_address_absolute_is_ctrl(address);
}

/**
 * @brief Get the pxn of an absolute address
 * 
 * @param address The absolute address
 * @return uintptr_t The PXN of the address
 */
static inline uintptr_t ph_address_absolute_pxn(uintptr_t address)
{
    return ph_get_bits_under_mask_shift(address, ABSOLUTE_PXN_MASK, ABSOLUTE_PXN_LO);
}

/**
 * @brief Set the PXN of an absolute address
 */
static inline uintptr_t ph_address_absolute_set_pxn(uintptr_t address, uintptr_t pxn)
{
    return ph_set_bits_under_mask_shift(address, ABSOLUTE_PXN_MASK, pxn, ABSOLUTE_PXN_LO);
}

/**
 * @brief Get the pod of an absolute address
 * 
 * @param address The absolute address
 * @return uintptr_t The pod of the address
 */
static inline uintptr_t ph_address_absolute_pod(uintptr_t address)
{
    return ph_get_bits_under_mask_shift(address, ABSOLUTE_POD_MASK, ABSOLUTE_POD_LO);
}

/**
 * @brief Set the pod of an absolute address
 */
static inline uintptr_t ph_address_absolute_set_pod(uintptr_t address, uintptr_t pod)
{
    return ph_set_bits_under_mask_shift(address, ABSOLUTE_POD_MASK, pod, ABSOLUTE_POD_LO);
}

/**
 * @brief Get the core of an absolute address
 * 
 * @param address The absolute address
 * @return uintptr_t The core of the address
 */
static inline uintptr_t ph_address_absolute_core(uintptr_t address)
{
    return ph_get_bits_under_mask_shift(address, ABSOLUTE_CORE_MASK, ABSOLUTE_CORE_LO);
}

/**
 * @brief Set the core of an absolute address
 */
static inline uintptr_t ph_address_absolute_set_core(uintptr_t address, uintptr_t core)
{
    return ph_set_bits_under_mask_shift(address, ABSOLUTE_CORE_MASK, core, ABSOLUTE_CORE_LO);
};

/**
 * @brief Get the dram offset of an absolute address
 * 
 * @param address The absolute address
 * @return uintptr_t The dram offset of the address
 */
static inline uintptr_t ph_address_absolute_dram_offset(uintptr_t address)
{
    return ph_get_bits_under_mask_shift(address, ABSOLUTE_DRAM_OFFSET_MASK, ABSOLUTE_DRAM_OFFSET_LO);
}

/**
 * @brief Set the dram offset of an absolute address
 *
 * @param address The absolute address
 * @param dram_offset The dram offset to set
 */
static inline uintptr_t ph_address_absolute_set_dram_offset(uintptr_t address, uintptr_t dram_offset)
{
    return ph_set_bits_under_mask_shift(address, ABSOLUTE_DRAM_OFFSET_MASK, dram_offset, ABSOLUTE_DRAM_OFFSET_LO);
}

/**
 * @brief Get the l2sp offset of an absolute address
 * 
 * @param address The absolute address
 * @return uintptr_t The l2sp offset of the address
 */
static inline uintptr_t ph_address_absolute_l2sp_offset(uintptr_t address)
{
    return ph_get_bits_under_mask_shift(address, ABSOLUTE_L2SP_OFFSET_MASK, ABSOLUTE_L2SP_OFFSET_LO);
}

/**
 * @brief Set the l2sp offset of an absolute address
 *
 * @param address The absolute address
 * @param l2sp_offset The l2sp offset to set
 */
static inline uintptr_t ph_address_absolute_set_l2sp_offset(uintptr_t address, uintptr_t l2sp_offset)
{
    return ph_set_bits_under_mask_shift(address, ABSOLUTE_L2SP_OFFSET_MASK, l2sp_offset, ABSOLUTE_L2SP_OFFSET_LO);
}

/**
 * @brief Get the control register offset of an absolute address
 * 
 * @param address The absolute address
 * @return uintptr_t The control register offset of the address
 */
static inline uintptr_t ph_address_absolute_ctrl_offset(uintptr_t address)
{
    return ph_get_bits_under_mask_shift(address, ABSOLUTE_CTRL_OFFSET_MASK, ABSOLUTE_CTRL_OFFSET_LO);
}

/**
 * @brief Get the l1sp offset of an absolute address
 * 
 * @param address The absolute address
 * @return uintptr_t The l1sp offset of the address
 */
static inline uintptr_t ph_address_absolute_l1sp_offset(uintptr_t address)
{
    return ph_get_bits_under_mask_shift(address, ABSOLUTE_L1SP_OFFSET_MASK, ABSOLUTE_L1SP_OFFSET_LO);
}

/**
 * @brief Set the l1sp offset of an absolute address
 *
 * @param address The absolute address
 * @param offset The new l1sp offset
 * @return uintptr_t The new absolute address
 */
static inline uintptr_t ph_address_absolute_set_l1sp_offset(uintptr_t address, uintptr_t offset)
{
    return ph_set_bits_under_mask_shift(address, ABSOLUTE_L1SP_OFFSET_MASK, offset, ABSOLUTE_L1SP_OFFSET_LO);
}

/**
 * @brief Check if a relative address is DRAM
 */
#define RELATIVE_IS_DRAM_MASK PH_MAKE_MASK(RELATIVE_IS_DRAM_HI, RELATIVE_IS_DRAM_LO)

/**
 * @brief Check if a relative address is L2SP
 */
#define RELATIVE_IS_L2SP_MASK PH_MAKE_MASK(RELATIVE_IS_L2SP_HI, RELATIVE_IS_L2SP_LO)


/**
 * @brief Mask of L1SP offset in a relative address
 */
#define RELATIVE_L1SP_OFFSET_MASK PH_MAKE_MASK(RELATIVE_L1SP_OFFSET_HI, RELATIVE_L1SP_OFFSET_LO)

/**
 * @brief Mask of L2SP offset in a relative address
 */
#define RELATIVE_L2SP_OFFSET_MASK PH_MAKE_MASK(RELATIVE_L2SP_OFFSET_HI, RELATIVE_L2SP_OFFSET_LO)

/**
 * @brief Mask of DRAM offset in a relative address
 */
#define RELATIVE_DRAM_OFFSET_MASK PH_MAKE_MASK(RELATIVE_DRAM_OFFSET_HI, RELATIVE_DRAM_OFFSET_LO)

/**
 * @brief Check if a relative address is DRAM
 *
 * @param address The relative address to check
 * @return uintptr_t nonzero if the address is DRAM, 0 otherwise
 */
static inline uintptr_t ph_address_relative_is_dram(uintptr_t address)
{
    return ph_get_bits_under_mask(address, RELATIVE_IS_DRAM_MASK);
}

/**
 * @brief Check if a relative address is L2SP
 * 
 * @param address The relative address to check
 * @return uintptr_t nonzero if the address is L2SP, 0 otherwise
 */
static inline uintptr_t ph_address_relative_is_l2sp(uintptr_t address)
{
    return ph_get_bits_under_mask(address, RELATIVE_IS_L2SP_MASK);
}

/**
 * @brief Check if a relative address is L1SP
 * 
 * @param address The relative address
 * @return uintptr_t nonzero if the address is L1SP, 0 otherwise
 */
static inline uintptr_t ph_address_relative_is_l1sp(uintptr_t address)
{
    return !ph_address_relative_is_dram(address) && !ph_address_relative_is_l2sp(address);
}

/**
 * @brief Get the dram offset of a relative address
 * 
 * @param address The relative address
 * @return uintptr_t The dram offset of the address
 */
static inline uintptr_t ph_address_relative_dram_offset(uintptr_t address)
{
    return ph_get_bits_under_mask_shift(address, RELATIVE_DRAM_OFFSET_MASK, RELATIVE_DRAM_OFFSET_LO);
}

/**
 * @brief Get the l2sp offset of a relative address
 * 
 * @param address The relative address
 * @return uintptr_t The l2sp offset of the address
 */
static inline uintptr_t ph_address_relative_l2sp_offset(uintptr_t address)
{
    return ph_get_bits_under_mask_shift(address, RELATIVE_L2SP_OFFSET_MASK, RELATIVE_L2SP_OFFSET_LO);
}

/**
 * @brief Get the l1sp offset of a relative address
 * 
 * @param address The relative address
 * @return uintptr_t The l1sp offset of the address
 */
static inline uintptr_t ph_address_relative_l1sp_offset(uintptr_t address)
{
    return ph_get_bits_under_mask_shift(address, RELATIVE_L1SP_OFFSET_MASK, RELATIVE_L1SP_OFFSET_LO);
}

/**
 * @brief Get the absolute address of a relative address
 * 
 * @param address The relative address
 * @param pxn The target pxn
 * @param pod The target pod
 * @param core The target core
 * @return uintptr_t The absolute address
 */
static inline uintptr_t ph_address_relative_l1sp_to_absolute(uintptr_t address,
                                                             uintptr_t pxn,
                                                             uintptr_t pod,
                                                             uintptr_t core)
{
    uintptr_t absolute = 0;
    absolute = ph_address_set_absolute(absolute, 1);
    absolute = ph_address_absolute_set_pxn(absolute, pxn);
    absolute = ph_address_absolute_set_pod(absolute, pod);
    absolute = ph_address_absolute_set_core(absolute, core);
    absolute = ph_address_absolute_set_l1sp_offset(absolute, ph_address_relative_l1sp_offset(address));
    return absolute;

}

static inline uintptr_t ph_address_relative_l2sp_to_absolute(uintptr_t rel,
                                                             uintptr_t pxn,
                                                             uintptr_t pod) {
    uintptr_t abs = 0;
    abs = ph_address_set_absolute(abs, 1);
    abs = ph_address_absolute_set_l2sp(abs, 1);
    abs = ph_address_absolute_set_pxn(abs, pxn);
    abs = ph_address_absolute_set_pod(abs, pod);
    abs = ph_address_absolute_set_l2sp_offset(abs, ph_address_relative_l2sp_offset(rel));
    return abs;
}

static inline uintptr_t ph_address_relative_dram_to_absolute(uintptr_t rel, uintptr_t pxn) {
    uintptr_t abs = 0;
    abs = ph_address_set_absolute(abs, 1);
    abs = ph_address_absolute_set_dram(abs, 1);
    abs = ph_address_absolute_set_pxn(abs, pxn);
    abs = ph_address_absolute_set_dram_offset(abs, ph_address_relative_dram_offset(rel));
    return abs;
}

#endif
