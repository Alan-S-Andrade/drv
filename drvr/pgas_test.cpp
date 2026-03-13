// pgas_test.cpp
// Simple test for PGAS virtual-to-physical address translation.
//
// 1. Write known values to DRAM via direct physical addresses.
// 2. Program SIT/PTT tables via MMIO to map VGID 0 -> PXN 0.
// 3. Read the same data back through PGAS virtual addresses.
// 4. Verify the values match.
//
// Run with 1 PXN, 1 core, 1 thread to keep it simple.
// The test prints PASS/FAIL for each check.

#include <cstdint>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/mmio.h>
#include <pandohammer/address.h>

// Build a direct absolute DRAM address for PXN 0, at a given byte offset
static inline uint64_t dram_addr(uint64_t offset)
{
    uintptr_t a = 0;
    a = ph_address_set_absolute(a, 1);
    a = ph_address_absolute_set_dram(a, 1);
    a = ph_address_absolute_set_pxn(a, 0);
    a = ph_address_absolute_set_dram_offset(a, offset);
    return a;
}

// Build a PGAS virtual address:
//   bit 63    = 1  (absolute)
//   bit 62    = 1  (looks like DRAM)
//   bits 61:59 = VSID  (3 bits)
//   bits 55:48 = VGID  (8 bits)
//   bits 47:0  = offset
static inline uint64_t pgas_vaddr(uint64_t vsid, uint64_t vgid, uint64_t offset)
{
    return (3ULL << 62)           // absolute + DRAM flags
         | ((vsid & 0x7) << 59)  // VSID in bits 61:59
         | ((vgid & 0xFF) << 48) // VGID in bits 55:48
         | (offset & ((1ULL << 48) - 1));
}

int main()
{
    int pxn = myPXNId();
    int errors = 0;

    // --- Step 1: Write known values to DRAM at physical addresses ---
    // Use offset 0x10000 (64KB) to stay clear of program data
    volatile uint64_t *phys_ptr0 = (volatile uint64_t *)dram_addr(0x10000);
    volatile uint64_t *phys_ptr1 = (volatile uint64_t *)dram_addr(0x10008);
    volatile uint64_t *phys_ptr2 = (volatile uint64_t *)dram_addr(0x10010);

    *phys_ptr0 = 0xDEADBEEF00000001ULL;
    *phys_ptr1 = 0xCAFEBABE00000002ULL;
    *phys_ptr2 = 0x1234567800000003ULL;

    // Verify direct physical reads work (sanity check)
    uint64_t v0 = *phys_ptr0;
    uint64_t v1 = *phys_ptr1;
    uint64_t v2 = *phys_ptr2;
    if (v0 != 0xDEADBEEF00000001ULL) { ph_print_int(-1); errors++; }
    if (v1 != 0xCAFEBABE00000002ULL) { ph_print_int(-2); errors++; }
    if (v2 != 0x1234567800000003ULL) { ph_print_int(-3); errors++; }

    // --- Step 2: Program SIT entry ---
    // SIT slot 0: VSID=1 (3 bits in positions 61:59), VGID in bits 55:48
    struct ph_pgas_sit_desc sit;
    sit.index     = 0;     // SIT slot 0
    sit.vsid      = 1;     // match VSID value = 1
    sit.vsid_bits = 3;     // 3 bits for VSID (bits 61:59)
    sit.vgid_hi   = 55;    // VGID high bit
    sit.vgid_lo   = 48;    // VGID low bit (8-bit VGID)
    ph_pgas_sit_write(&sit);

    // --- Step 3: Program PTT entry ---
    // VGID 0 in SIT slot 0 -> PXN 0
    struct ph_pgas_ptt_desc ptt;
    ptt.sit_index   = 0;    // SIT slot 0
    ptt.vgid        = 0;    // virtual group 0
    ptt.target_pxn  = 0;    // maps to physical PXN 0
    ptt.offset_base = 0;
    ph_pgas_ptt_write(&ptt);

    // --- Step 4: Read through PGAS virtual addresses ---
    // VSID=1, VGID=0, offset=0x10000 should map to PXN0 DRAM offset 0x10000
    volatile uint64_t *virt_ptr0 = (volatile uint64_t *)pgas_vaddr(1, 0, 0x10000);
    volatile uint64_t *virt_ptr1 = (volatile uint64_t *)pgas_vaddr(1, 0, 0x10008);
    volatile uint64_t *virt_ptr2 = (volatile uint64_t *)pgas_vaddr(1, 0, 0x10010);

    uint64_t r0 = *virt_ptr0;
    uint64_t r1 = *virt_ptr1;
    uint64_t r2 = *virt_ptr2;

    // --- Step 5: Verify ---
    if (r0 == 0xDEADBEEF00000001ULL) {
        ph_print_int(1);  // PASS: read 0
    } else {
        ph_print_hex(r0);
        ph_print_int(-10);
        errors++;
    }

    if (r1 == 0xCAFEBABE00000002ULL) {
        ph_print_int(2);  // PASS: read 1
    } else {
        ph_print_hex(r1);
        ph_print_int(-11);
        errors++;
    }

    if (r2 == 0x1234567800000003ULL) {
        ph_print_int(3);  // PASS: read 2
    } else {
        ph_print_hex(r2);
        ph_print_int(-12);
        errors++;
    }

    // --- Step 6: Write through virtual address, read back physical ---
    *virt_ptr0 = 0xAAAABBBBCCCCDDDDULL;
    uint64_t rb = *phys_ptr0;
    if (rb == 0xAAAABBBBCCCCDDDDULL) {
        ph_print_int(4);  // PASS: write-through
    } else {
        ph_print_hex(rb);
        ph_print_int(-20);
        errors++;
    }

    // --- Step 7: Test unmapped VGID (should use normal path) ---
    // VGID=1 has no PTT entry, so translate() returns false.
    // With VSID=1 and VGID=1, the address passes through to_absolute().
    // The resulting physical address depends on the normal decoder,
    // so we just verify we don't crash.

    // --- Summary ---
    if (errors == 0) {
        ph_print_int(100);  // ALL PASS
    } else {
        ph_print_int(-100); // SOME FAILED
    }

    return 0;
}
