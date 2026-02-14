import csv
import sys
import math
import collections

# Core statistics we care about
CORE_STATS = [
    "busy_cycles",
    "memory_wait_cycles",
    "active_idle_cycles",
    "load_dram",
    "load_l1sp",
    "load_l2sp",
    "store_dram",
    "store_l1sp",
    "store_l2sp",
    "atomic_l1sp",
    "atomic_l2sp",
    "atomic_dram",
    "icache_miss",
    "load_latency_total",
    "load_request_count",
    "dram_load_latency_total",
    "dram_load_request_count",
    "useful_load_l1sp",
    "useful_store_l1sp",
    "useful_atomic_l1sp",
    "useful_load_l2sp",
    "useful_store_l2sp",
    "useful_atomic_l2sp",
    "useful_load_dram",
    "useful_store_dram",
    "useful_atomic_dram",
    "useful_busy_cycles",
    "useful_memory_wait_cycles",
    "useful_active_idle_cycles",
    "useful_load_latency_total",
    "useful_load_request_count",
    "useful_dram_load_latency_total",
    "useful_dram_load_request_count",
]

# DRAM cache statistics
CACHE_STATS = [
    "CacheHits",
    "CacheMisses",
    "latency_GetS_hit",
    "latency_GetX_hit",
    "latency_GetS_miss",
    "latency_GetX_miss",
]

# MemController statistics (SST built-in + custom)
MEMCTRL_STATS = [
    "outstanding_requests",
    "cycles_with_issue",
    "cycles_attempted_issue_but_rejected",
    "total_cycles",
    "requests_received_GetS",
    "requests_received_GetX",
    "requests_received_Write",
    "latency_GetS",
    "latency_Write",
]

def is_dram_cache(name):
    """Check if component is a DRAM cache (victim_cache or dram*_cache)"""
    return "victim_cache" in name or ("dram" in name and "_cache" in name)

def short_core_name(full_name):
    """Convert system_pxn0_pod0_core0_core to pxn0_pod0_core0"""
    name = full_name.replace("system_", "").replace("_core_core", "").replace("_core", "")
    # Handle hostcore
    if "hostcore" in name:
        return name
    return name

def short_cache_name(full_name):
    """Convert system_pxn0_dram0_cache to pxn0_dram0"""
    return full_name.replace("system_", "").replace("_cache", "")

def short_memctrl_name(full_name):
    """Convert system_pxn0_pod0_l2sp0_memctrl to pxn0_pod0_l2sp0"""
    return full_name.replace("system_", "").replace("_memctrl", "")

def main():
    core_stats = collections.defaultdict(lambda: collections.defaultdict(int))
    cache_stats = collections.defaultdict(lambda: collections.defaultdict(lambda: {"sum": 0, "count": 0}))
    memctrl_stats = collections.defaultdict(lambda: collections.defaultdict(lambda: {"sum": 0, "sumsq": 0, "count": 0, "min": 0, "max": 0}))

    # 1. Read the CSV produced by SST
    try:
        with open("stats.csv", "r") as f:
            reader = csv.DictReader(f)
            for row in reader:
                full_name = row["ComponentName"]
                stat_name = row["StatisticName"]

                # Core statistics (exclude cache components)
                if "core" in full_name and "_cache" not in full_name:
                    core_id = full_name
                    if stat_name in CORE_STATS:
                        core_stats[core_id][stat_name] += int(row["Sum.u64"])

                # DRAM cache statistics
                if is_dram_cache(full_name):
                    cache_id = full_name
                    if stat_name in CACHE_STATS:
                        cache_stats[cache_id][stat_name]["sum"] += int(row["Sum.u64"])
                        cache_stats[cache_id][stat_name]["count"] += int(row["Count.u64"])

                # MemController stats (L1SP, L2SP, DRAM)
                if "memctrl" in full_name and stat_name in MEMCTRL_STATS:
                    memctrl_stats[full_name][stat_name]["sum"] += int(row["Sum.u64"])
                    memctrl_stats[full_name][stat_name]["sumsq"] += int(row["SumSQ.u64"])
                    memctrl_stats[full_name][stat_name]["count"] += int(row["Count.u64"])
                    memctrl_stats[full_name][stat_name]["min"] = int(row["Min.u64"])
                    memctrl_stats[full_name][stat_name]["max"] = int(row["Max.u64"])

    except FileNotFoundError:
        print("Error: 'stats.csv' not found. Did you run the simulation?")
        sys.exit(1)

    # 2. Aggregate totals
    total_load_l1sp = sum(d["load_l1sp"] for d in core_stats.values())
    total_load_l2sp = sum(d["load_l2sp"] for d in core_stats.values())
    total_load_dram = sum(d["load_dram"] for d in core_stats.values())
    total_store_l1sp = sum(d["store_l1sp"] for d in core_stats.values())
    total_store_l2sp = sum(d["store_l2sp"] for d in core_stats.values())
    total_store_dram = sum(d["store_dram"] for d in core_stats.values())
    total_atomic_l1sp = sum(d["atomic_l1sp"] for d in core_stats.values())
    total_atomic_l2sp = sum(d["atomic_l2sp"] for d in core_stats.values())
    total_atomic_dram = sum(d["atomic_dram"] for d in core_stats.values())

    # Useful stats
    total_useful_load_l1sp = sum(d["useful_load_l1sp"] for d in core_stats.values())
    total_useful_store_l1sp = sum(d["useful_store_l1sp"] for d in core_stats.values())
    total_useful_atomic_l1sp = sum(d["useful_atomic_l1sp"] for d in core_stats.values())
    total_useful_load_l2sp = sum(d["useful_load_l2sp"] for d in core_stats.values())
    total_useful_store_l2sp = sum(d["useful_store_l2sp"] for d in core_stats.values())
    total_useful_atomic_l2sp = sum(d["useful_atomic_l2sp"] for d in core_stats.values())
    total_useful_load_dram = sum(d["useful_load_dram"] for d in core_stats.values())
    total_useful_store_dram = sum(d["useful_store_dram"] for d in core_stats.values())
    total_useful_atomic_dram = sum(d["useful_atomic_dram"] for d in core_stats.values())

    total_cache_hits = sum(d["CacheHits"]["sum"] for d in cache_stats.values())
    total_cache_misses = sum(d["CacheMisses"]["sum"] for d in cache_stats.values())

    # Cache-local latencies (measured at cache, excludes interconnect)
    total_hit_lat_sum = sum(d["latency_GetS_hit"]["sum"] + d["latency_GetX_hit"]["sum"] for d in cache_stats.values())
    total_hit_lat_count = sum(d["latency_GetS_hit"]["count"] + d["latency_GetX_hit"]["count"] for d in cache_stats.values())

    total_miss_lat_sum = sum(d["latency_GetS_miss"]["sum"] + d["latency_GetX_miss"]["sum"] for d in cache_stats.values())
    total_miss_lat_count = sum(d["latency_GetS_miss"]["count"] + d["latency_GetX_miss"]["count"] for d in cache_stats.values())

    # Core-perspective latencies (end-to-end, includes interconnect)
    total_dram_lat_sum = sum(d["dram_load_latency_total"] for d in core_stats.values())
    total_dram_lat_count = sum(d["dram_load_request_count"] for d in core_stats.values())
    total_load_lat_sum = sum(d["load_latency_total"] for d in core_stats.values())
    total_load_lat_count = sum(d["load_request_count"] for d in core_stats.values())

    # Useful-phase latency aggregates
    useful_load_lat_sum = sum(d["useful_load_latency_total"] for d in core_stats.values())
    useful_load_lat_count = sum(d["useful_load_request_count"] for d in core_stats.values())
    useful_dram_lat_sum = sum(d["useful_dram_load_latency_total"] for d in core_stats.values())
    useful_dram_lat_count = sum(d["useful_dram_load_request_count"] for d in core_stats.values())

    # Useful-phase cycle aggregates (across all cores)
    total_useful_busy = sum(d["useful_busy_cycles"] for d in core_stats.values())
    total_useful_memwait = sum(d["useful_memory_wait_cycles"] for d in core_stats.values())
    total_useful_idle = sum(d["useful_active_idle_cycles"] for d in core_stats.values())

    # Useful phase duration: max across cores (the phase window length)
    useful_phase_per_core = []
    for d in core_stats.values():
        phase_cyc = d["useful_busy_cycles"] + d["useful_memory_wait_cycles"] + d["useful_active_idle_cycles"]
        if phase_cyc > 0:
            useful_phase_per_core.append(phase_cyc)
    max_useful_phase_cycles = max(useful_phase_per_core) if useful_phase_per_core else 0

    # Total simulation cycles (from any core)
    total_sim_per_core = []
    for d in core_stats.values():
        sim_cyc = d["busy_cycles"] + d["memory_wait_cycles"] + d["active_idle_cycles"]
        if sim_cyc > 0:
            total_sim_per_core.append(sim_cyc)
    max_sim_cycles = max(total_sim_per_core) if total_sim_per_core else 0

    # 3. Print Summary Statistics
    print("=" * 90)
    print("MEMORY ACCESS SUMMARY (PANDOHammer)")
    print("=" * 90)

    total_mem_accesses = (total_load_l1sp + total_load_l2sp + total_load_dram +
                          total_store_l1sp + total_store_l2sp + total_store_dram)
    total_cache_accesses = total_cache_hits + total_cache_misses

    print(f"\n{'Metric':<45} {'Value':<15} {'Details':<25}")
    print("-" * 90)

    # L1 Scratchpad stats
    l1sp_accesses = total_load_l1sp + total_store_l1sp
    l1sp_pct = (l1sp_accesses / total_mem_accesses * 100) if total_mem_accesses > 0 else 0
    print(f"{'L1 Scratchpad (L1SP) Accesses':<45} {l1sp_accesses:<15} {l1sp_pct:.1f}% of all accesses")
    print(f"  - Loads{'':<39} {total_load_l1sp:<15}")
    print(f"  - Stores{'':<38} {total_store_l1sp:<15}")

    # L2 Scratchpad stats
    l2sp_accesses = total_load_l2sp + total_store_l2sp
    l2sp_pct = (l2sp_accesses / total_mem_accesses * 100) if total_mem_accesses > 0 else 0
    print(f"\n{'L2 Scratchpad (L2SP) Accesses':<45} {l2sp_accesses:<15} {l2sp_pct:.1f}% of all accesses")
    print(f"  - Loads{'':<39} {total_load_l2sp:<15}")
    print(f"  - Stores{'':<38} {total_store_l2sp:<15}")

    # DRAM address space stats (goes through DRAM cache)
    dram_space_accesses = total_load_dram + total_store_dram
    dram_space_pct = (dram_space_accesses / total_mem_accesses * 100) if total_mem_accesses > 0 else 0
    print(f"\n{'DRAM Address Space Accesses':<45} {dram_space_accesses:<15} {dram_space_pct:.1f}% of all accesses")
    print(f"  - Loads{'':<39} {total_load_dram:<15}")
    print(f"  - Stores{'':<38} {total_store_dram:<15}")

    # Useful accesses breakdown
    print(f"\n{'--- Useful vs Total Accesses (stat_phase filtering) ---':<45}")

    total_l1sp = total_load_l1sp + total_store_l1sp + total_atomic_l1sp
    useful_l1sp = total_useful_load_l1sp + total_useful_store_l1sp + total_useful_atomic_l1sp
    overhead_l1sp_pct = ((total_l1sp - useful_l1sp) / total_l1sp * 100) if total_l1sp > 0 else 0
    print(f"{'L1SP Total':<30} {total_l1sp:<12} {'Useful:':<10} {useful_l1sp:<12} {'Overhead:':<10} {overhead_l1sp_pct:.1f}%")
    print(f"  - Loads:  total={total_load_l1sp:<10} useful={total_useful_load_l1sp:<10}")
    print(f"  - Stores: total={total_store_l1sp:<10} useful={total_useful_store_l1sp:<10}")
    print(f"  - Atomic: total={total_atomic_l1sp:<10} useful={total_useful_atomic_l1sp:<10}")

    total_l2sp = total_load_l2sp + total_store_l2sp + total_atomic_l2sp
    useful_l2sp = total_useful_load_l2sp + total_useful_store_l2sp + total_useful_atomic_l2sp
    overhead_l2sp_pct = ((total_l2sp - useful_l2sp) / total_l2sp * 100) if total_l2sp > 0 else 0
    print(f"{'L2SP Total':<30} {total_l2sp:<12} {'Useful:':<10} {useful_l2sp:<12} {'Overhead:':<10} {overhead_l2sp_pct:.1f}%")
    print(f"  - Loads:  total={total_load_l2sp:<10} useful={total_useful_load_l2sp:<10}")
    print(f"  - Stores: total={total_store_l2sp:<10} useful={total_useful_store_l2sp:<10}")
    print(f"  - Atomic: total={total_atomic_l2sp:<10} useful={total_useful_atomic_l2sp:<10}")

    total_dram_all = total_load_dram + total_store_dram + total_atomic_dram
    useful_dram = total_useful_load_dram + total_useful_store_dram + total_useful_atomic_dram
    overhead_dram_pct = ((total_dram_all - useful_dram) / total_dram_all * 100) if total_dram_all > 0 else 0
    print(f"{'DRAM Total':<30} {total_dram_all:<12} {'Useful:':<10} {useful_dram:<12} {'Overhead:':<10} {overhead_dram_pct:.1f}%")
    print(f"  - Loads:  total={total_load_dram:<10} useful={total_useful_load_dram:<10}")
    print(f"  - Stores: total={total_store_dram:<10} useful={total_useful_store_dram:<10}")
    print(f"  - Atomic: total={total_atomic_dram:<10} useful={total_useful_atomic_dram:<10}")

    # DRAM cache stats
    cache_hit_rate = (total_cache_hits / total_cache_accesses * 100) if total_cache_accesses > 0 else 0

    # Cache-local latencies (excludes interconnect - measured at cache component)
    cache_local_hit_lat = total_hit_lat_sum / total_hit_lat_count if total_hit_lat_count > 0 else 0
    cache_local_miss_lat = total_miss_lat_sum / total_miss_lat_count if total_miss_lat_count > 0 else 0

    # Core-perspective latencies (includes interconnect - measured at core)
    avg_dram_lat = total_dram_lat_sum / total_dram_lat_count if total_dram_lat_count > 0 else 0
    avg_load_lat = total_load_lat_sum / total_load_lat_count if total_load_lat_count > 0 else 0

    # Estimate interconnect overhead: difference between core-perspective and cache-local average
    # Core sees: hits * hit_lat + misses * miss_lat = total_dram_lat
    # Cache sees: hits * cache_hit_lat + misses * cache_miss_lat = cache_total_lat
    cache_local_avg = (total_hit_lat_sum + total_miss_lat_sum) / (total_hit_lat_count + total_miss_lat_count) if (total_hit_lat_count + total_miss_lat_count) > 0 else 0
    interconnect_overhead = avg_dram_lat - cache_local_avg if cache_local_avg > 0 else 0

    # Core-perspective hit/miss latencies (cache-local + interconnect)
    core_hit_lat = cache_local_hit_lat + interconnect_overhead
    core_miss_lat = cache_local_miss_lat + interconnect_overhead

    print(f"\n{'DRAM Cache Hits':<45} {total_cache_hits:<15} {cache_hit_rate:.1f}% hit rate")
    print(f"{'DRAM Cache Misses (actual DRAM accesses)':<45} {total_cache_misses:<15}")
    print(f"\n{'--- Core-Perspective Latencies (includes interconnect) ---':<45}")
    print(f"{'  Avg DRAM Load Latency (end-to-end)':<45} {avg_dram_lat:.1f} cycles")
    print(f"{'  Estimated Cache Hit Latency':<45} {core_hit_lat:.1f} cycles")
    print(f"{'  Estimated Cache Miss Latency':<45} {core_miss_lat:.1f} cycles")
    print(f"{'  Interconnect Overhead (round-trip)':<45} {interconnect_overhead:.1f} cycles")
    print(f"\n{'--- Cache-Local Latencies (excludes interconnect) ---':<45}")
    print(f"{'  Cache Hit Latency':<45} {cache_local_hit_lat:.1f} cycles")
    print(f"{'  Cache Miss Latency':<45} {cache_local_miss_lat:.1f} cycles")

    # Overall memory stats
    print(f"\n{'--- Overall Memory Performance ---':<45}")
    print(f"{'  Avg Load Latency (all memory types)':<45} {avg_load_lat:.1f} cycles")

    # Useful-phase latencies
    useful_avg_load_lat = useful_load_lat_sum / useful_load_lat_count if useful_load_lat_count > 0 else 0
    useful_avg_dram_lat = useful_dram_lat_sum / useful_dram_lat_count if useful_dram_lat_count > 0 else 0

    print(f"\n{'--- Useful Phase Memory Performance (stat_phase=1 only) ---':<45}")
    print(f"{'  Useful Phase Duration (max core)':<45} {max_useful_phase_cycles:<15} cycles ({max_useful_phase_cycles/1e6:.3f} ms)")
    print(f"{'  Total Simulation Duration (max core)':<45} {max_sim_cycles:<15} cycles ({max_sim_cycles/1e6:.3f} ms)")
    startup_pct = ((max_sim_cycles - max_useful_phase_cycles) / max_sim_cycles * 100) if max_sim_cycles > 0 else 0
    print(f"{'  Startup/Teardown Overhead':<45} {startup_pct:.1f}%")
    print(f"{'  Avg Load Latency (useful phase)':<45} {useful_avg_load_lat:.1f} cycles")
    if useful_dram_lat_count > 0:
        print(f"{'  Avg DRAM Load Latency (useful phase)':<45} {useful_avg_dram_lat:.1f} cycles")

    # 4. Print Per-Core Statistics (Total and Useful Phase)
    print("\n" + "=" * 170)
    print("PER-CORE STATISTICS (Total | Useful Phase)")
    print("=" * 170)
    headers = ["Core",
               "Busy %", "MemWait %", "Idle %",
               "uBusy %", "uMemW %", "uIdle %",
               "L1SP Ld", "L2SP Ld", "DRAM Ld", "I-Miss"]
    print(f"{headers[0]:<25} "
          f"{headers[1]:<10} {headers[2]:<10} {headers[3]:<10} "
          f"{headers[4]:<10} {headers[5]:<10} {headers[6]:<10} "
          f"{headers[7]:<12} {headers[8]:<12} {headers[9]:<12} {headers[10]:<10}")
    print("-" * 170)

    for core in sorted(core_stats.keys()):
        d = core_stats[core]
        total_active = d["busy_cycles"] + d["memory_wait_cycles"] + d["active_idle_cycles"]
        if total_active == 0: total_active = 1

        busy_pct = (d["busy_cycles"] / total_active) * 100
        wait_pct = (d["memory_wait_cycles"] / total_active) * 100
        idle_pct = (d["active_idle_cycles"] / total_active) * 100

        # Useful-phase percentages
        useful_active = d["useful_busy_cycles"] + d["useful_memory_wait_cycles"] + d["useful_active_idle_cycles"]
        if useful_active == 0: useful_active = 1
        u_busy_pct = (d["useful_busy_cycles"] / useful_active) * 100
        u_wait_pct = (d["useful_memory_wait_cycles"] / useful_active) * 100
        u_idle_pct = (d["useful_active_idle_cycles"] / useful_active) * 100

        display_name = short_core_name(core)
        print(f"{display_name:<25} "
              f"{busy_pct:<10.1f} {wait_pct:<10.1f} {idle_pct:<10.1f} "
              f"{u_busy_pct:<10.1f} {u_wait_pct:<10.1f} {u_idle_pct:<10.1f} "
              f"{d['load_l1sp']:<12} {d['load_l2sp']:<12} {d['load_dram']:<12} {d['icache_miss']:<10}")

    # 5. Print DRAM Cache Table
    if cache_stats:
        print("\n" + "=" * 100)
        print("PER-DRAM-CACHE STATISTICS (cache-local latencies, add ~{:.0f} cycles for core-perspective)".format(interconnect_overhead))
        print("=" * 100)
        headers = ["DRAM Cache", "Hits", "Misses", "Hit %", "Hit Lat (cyc)", "Miss Lat (cyc)"]
        print(f"{headers[0]:<20} {headers[1]:<12} {headers[2]:<12} {headers[3]:<10} {headers[4]:<15} {headers[5]:<15}")
        print("-" * 100)

        for cache in sorted(cache_stats.keys()):
            d = cache_stats[cache]
            hits = d["CacheHits"]["sum"]
            misses = d["CacheMisses"]["sum"]
            total = hits + misses
            hit_rate = (hits / total * 100) if total > 0 else 0

            hit_lat_sum = d["latency_GetS_hit"]["sum"] + d["latency_GetX_hit"]["sum"]
            hit_lat_count = d["latency_GetS_hit"]["count"] + d["latency_GetX_hit"]["count"]
            avg_hit = hit_lat_sum / hit_lat_count if hit_lat_count > 0 else 0

            miss_lat_sum = d["latency_GetS_miss"]["sum"] + d["latency_GetX_miss"]["sum"]
            miss_lat_count = d["latency_GetS_miss"]["count"] + d["latency_GetX_miss"]["count"]
            avg_miss = miss_lat_sum / miss_lat_count if miss_lat_count > 0 else 0

            display_name = short_cache_name(cache)
            print(f"{display_name:<20} {hits:<12} {misses:<12} {hit_rate:<10.1f} {avg_hit:<15.1f} {avg_miss:<15.1f}")

    # 6. Print MemController Statistics (L2SP, L1SP, DRAM)
    def compute_stddev(sumsq, sumv, count):
        """Compute stddev from SumSQ, Sum, and Count."""
        if count < 2:
            return 0.0
        mean = sumv / count
        variance = (sumsq / count) - (mean * mean)
        return math.sqrt(max(variance, 0.0))

    def print_memctrl_table(title, banks, useful_phase_cycles=0, useful_total_reqs=0):
        if not banks:
            return
        num_banks = len(banks)
        useful_reqs_per_bank = useful_total_reqs / num_banks if num_banks > 0 else 0
        print("\n" + "=" * 210)
        print(f"{title}")
        print("=" * 210)
        headers = ["Bank", "Reads", "Writes", "Util %", "uUtil %",
                   "Rejected",
                   "Avg Queue", "Max Queue", "Queue SD",
                   "Avg Rd Lat", "Min Rd Lat", "Max Rd Lat", "Rd Lat SD",
                   "Avg Wr Lat", "Avg IAT", "uIAT"]
        print(f"{headers[0]:<28} {headers[1]:<10} {headers[2]:<10} {headers[3]:<8} {headers[4]:<8} "
              f"{headers[5]:<10} "
              f"{headers[6]:<10} {headers[7]:<10} {headers[8]:<10} "
              f"{headers[9]:<10} {headers[10]:<10} {headers[11]:<10} {headers[12]:<10} "
              f"{headers[13]:<10} {headers[14]:<10} {headers[15]:<10}")
        print("-" * 210)
        for bank in sorted(banks.keys()):
            d = banks[bank]
            reads = d["requests_received_GetS"]["sum"]
            writes = d["requests_received_Write"]["sum"]
            total_cyc = d["total_cycles"]["sum"] if d["total_cycles"]["sum"] > 0 else 1
            issue_cyc = d["cycles_with_issue"]["sum"]
            util = (issue_cyc / total_cyc) * 100
            rejected = d["cycles_attempted_issue_but_rejected"]["sum"]

            # Useful-phase utilization: core-side useful requests / useful_phase_cycles
            # Distributed evenly across banks (assumes interleaved addressing)
            u_util = (useful_reqs_per_bank / useful_phase_cycles * 100) if useful_phase_cycles > 0 else 0

            # Queue depth stats
            oq = d["outstanding_requests"]
            avg_q = oq["sum"] / oq["count"] if oq["count"] > 0 else 0
            max_q = oq["max"]
            q_sd = compute_stddev(oq["sumsq"], oq["sum"], oq["count"])

            # Read latency stats
            rl = d["latency_GetS"]
            avg_rd = rl["sum"] / rl["count"] if rl["count"] > 0 else 0
            min_rd = rl["min"] if rl["count"] > 0 else 0
            max_rd = rl["max"] if rl["count"] > 0 else 0
            rd_sd = compute_stddev(rl["sumsq"], rl["sum"], rl["count"])

            # Write latency stats
            avg_wr = d["latency_Write"]["sum"] / d["latency_Write"]["count"] if d["latency_Write"]["count"] > 0 else 0

            # Average inter-arrival time (total cycles / total requests)
            total_reqs = reads + writes
            avg_iat = total_cyc / total_reqs if total_reqs > 0 else 0
            # Useful IAT: useful phase cycles / useful requests per bank
            u_iat = useful_phase_cycles / useful_reqs_per_bank if useful_reqs_per_bank > 0 else 0

            display = short_memctrl_name(bank)
            print(f"{display:<28} {reads:<10} {writes:<10} {util:<8.1f} {u_util:<8.1f} "
                  f"{rejected:<10} "
                  f"{avg_q:<10.2f} {max_q:<10} {q_sd:<10.2f} "
                  f"{avg_rd:<10.1f} {min_rd:<10} {max_rd:<10} {rd_sd:<10.2f} "
                  f"{avg_wr:<10.1f} {avg_iat:<10.2f} {u_iat:<10.2f}")

    if memctrl_stats:
        l2sp_mc = {k: v for k, v in memctrl_stats.items() if "l2sp" in k}
        l1sp_mc = {k: v for k, v in memctrl_stats.items() if "l1sp" in k}
        dram_mc = {k: v for k, v in memctrl_stats.items() if "dram" in k and "l2sp" not in k and "l1sp" not in k}

        useful_l2sp_reqs = total_useful_load_l2sp + total_useful_store_l2sp + total_useful_atomic_l2sp
        useful_l1sp_reqs = total_useful_load_l1sp + total_useful_store_l1sp + total_useful_atomic_l1sp
        useful_dram_reqs = total_useful_load_dram + total_useful_store_dram + total_useful_atomic_dram

        print_memctrl_table("L2SP BANK STATISTICS (per-bank MemController) [uUtil/uIAT = core-side useful-phase estimate]",
                            l2sp_mc, max_useful_phase_cycles, useful_l2sp_reqs)
        print_memctrl_table("L1SP BANK STATISTICS (per-bank MemController) [uUtil/uIAT = core-side useful-phase estimate]",
                            l1sp_mc, max_useful_phase_cycles, useful_l1sp_reqs)
        print_memctrl_table("DRAM BANK STATISTICS (per-bank MemController) [uUtil/uIAT = core-side useful-phase estimate]",
                            dram_mc, max_useful_phase_cycles, useful_dram_reqs)

if __name__ == "__main__":
    main()
