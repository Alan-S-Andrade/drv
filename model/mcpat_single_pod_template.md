McPAT pod template notes

Files:
- [mcpat_single_pod_template.xml](/users/alanandr/drv/model/mcpat_single_pod_template.xml)

How this maps to the DRV pod model:
- Pod topology comes from [pod.py](/users/alanandr/drv/model/pod.py): one single-router pod NoC, `cores + l2sp_banks + 1` ports.
- Core-level configuration comes from [compute.py](/users/alanandr/drv/model/compute.py): 1 GHz system/core clock in the exported sysconfig path.
- Default `L1SP` size comes from [memory.py](/users/alanandr/drv/model/memory.py): `256*1024` bytes per core.
- Default pod `L2SP` size and banking come from [pod.py](/users/alanandr/drv/model/pod.py): `1 MiB`, `1` bank by default.
- DRAM port count and interleave are exposed in [DrvAPISysConfig.hpp](/users/alanandr/drv/api/DrvAPISysConfig.hpp).

What you still need to fill in:
- `POD_CORES`, `CORE_THREADS`, `POD_L2SP_BANKS`, `PXN_DRAM_PORTS`
- technology node and clock target if not using the repo defaults
- microarchitectural widths for the core section
- activity counters from SST or postprocessed stats

Recommended activity mapping:
- `L1SP_READS_PER_CORE` / `L1SP_WRITES_PER_CORE`: from per-core scratchpad controller accesses
- `L2SP_BANK_READS` / `L2SP_BANK_WRITES`: from pod L2 scratchpad bank controllers
- `MC_READS` / `MC_WRITES`: from DRAM controller or Ramulator summaries
- `NOC_TOTAL_ACCESSES`: total packets or flits traversing the pod router
- `CORE_LOADS`, `CORE_STORES`, `CORE_TOTAL_INSTRUCTIONS`, `CORE_BUSY_CYCLES`: from the core model stats

Modeling note:
- McPAT has no first-class scratchpad primitive. This template models `L1SP` and `L2SP` as SRAM-backed cache-like arrays so CACTI-backed area/power estimation is still usable.
- For absolute fidelity you would calibrate the line size, associativity, and access counts to behave like scratchpads rather than coherent caches.
