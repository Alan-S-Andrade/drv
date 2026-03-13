// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#pragma once
#include <cstdint>
#include <cstring>
#include <DrvAPIAddressMap.hpp>

namespace DrvAPI {

struct SITEntry {
    bool     valid = false;
    uint64_t vsid = 0;       // Segment ID value to match
    int      vsid_bits = 0;  // Number of top bits forming VSID (below bits 63/62)
    int      vgid_hi = 0;    // High bit position of VGID in remaining address
    int      vgid_lo = 0;    // Low bit position of VGID
};

struct PTTEntry {
    bool    valid = false;
    int64_t target_pxn = 0;    // Physical destination PXN
    uint64_t offset_base = 0;  // Base offset added to DRAM offset (for bank scatter)
};

class PGASTranslator {
public:
    static constexpr int MAX_SEGMENTS = 4;
    static constexpr int MAX_VGIDS = 256;

    /**
     * Returns true if vaddr matched a SIT entry and was translated.
     * On true, paddr_out is a fully-formed absolute DRAM address.
     *
     * Virtual address format (application convention):
     *   Bit 63     = 1  (absolute flag)
     *   Bit 62     = 1  (DRAM flag — SIT intercepts before normal decode)
     *   Bits [61 .. 62-vsid_bits] = VSID
     *   Bits [vgid_hi .. vgid_lo] = VGID (within the remaining address)
     *   Bits [vgid_lo-1 .. 0]     = offset
     */
    bool translate(uint64_t vaddr,
                   const DrvAPIAddressDecoder& decoder,
                   uint64_t& paddr_out) const {
        // Only consider absolute DRAM-looking addresses (bits 63 and 62 set)
        if ((vaddr >> 62) != 0x3) return false;

        for (int i = 0; i < MAX_SEGMENTS; i++) {
            if (!sit_[i].valid) continue;

            // Extract VSID: vsid_bits bits starting just below bit 62
            // i.e., bits [61 .. 62-vsid_bits]
            int vsid_shift = 62 - sit_[i].vsid_bits;
            uint64_t vsid_mask = ((1ULL << sit_[i].vsid_bits) - 1);
            uint64_t addr_vsid = (vaddr >> vsid_shift) & vsid_mask;

            if (addr_vsid != sit_[i].vsid) continue;

            // Extract VGID
            int vgid_width = sit_[i].vgid_hi - sit_[i].vgid_lo + 1;
            uint64_t vgid = (vaddr >> sit_[i].vgid_lo) & ((1ULL << vgid_width) - 1);
            if (vgid >= MAX_VGIDS || !ptt_[i][vgid].valid) continue;

            // Extract offset (bits below vgid_lo)
            uint64_t offset = (sit_[i].vgid_lo > 0)
                ? (vaddr & ((1ULL << sit_[i].vgid_lo) - 1))
                : 0;

            // Build absolute DRAM address using the decoder's encode()
            DrvAPIAddressInfo info;
            info.set_absolute(true)
                .set_dram()
                .set_pxn(ptt_[i][vgid].target_pxn)
                .set_offset(ptt_[i][vgid].offset_base + offset);
            paddr_out = decoder.encode(info);
            return true;
        }
        return false;  // No match — use existing path
    }

    void writeSIT(int index, const SITEntry& entry) {
        if (index >= 0 && index < MAX_SEGMENTS) {
            sit_[index] = entry;
        }
    }

    void writePTT(int sit_index, int vgid, const PTTEntry& entry) {
        if (sit_index >= 0 && sit_index < MAX_SEGMENTS &&
            vgid >= 0 && vgid < MAX_VGIDS) {
            ptt_[sit_index][vgid] = entry;
        }
    }

    void clear() {
        for (int i = 0; i < MAX_SEGMENTS; i++) {
            sit_[i] = SITEntry{};
            for (int j = 0; j < MAX_VGIDS; j++) {
                ptt_[i][j] = PTTEntry{};
            }
        }
    }

private:
    SITEntry sit_[MAX_SEGMENTS];
    PTTEntry ptt_[MAX_SEGMENTS][MAX_VGIDS];
};

} // namespace DrvAPI
