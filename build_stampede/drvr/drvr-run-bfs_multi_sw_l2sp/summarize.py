import csv
import sys
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

def main():
    core_stats = collections.defaultdict(lambda: collections.defaultdict(int))
    cache_stats = collections.defaultdict(lambda: collections.defaultdict(lambda: {"sum": 0, "count": 0}))

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

    # 4. Print Per-Core Statistics
    print("\n" + "=" * 120)
    print("PER-CORE STATISTICS")
    print("=" * 120)
    headers = ["Core", "Busy %", "MemWait %", "Idle %", "L1SP Ld", "L2SP Ld", "DRAM Ld", "I-Miss"]
    print(f"{headers[0]:<25} {headers[1]:<10} {headers[2]:<10} {headers[3]:<10} {headers[4]:<12} {headers[5]:<12} {headers[6]:<12} {headers[7]:<10}")
    print("-" * 120)

    for core in sorted(core_stats.keys()):
        d = core_stats[core]
        total_active = d["busy_cycles"] + d["memory_wait_cycles"] + d["active_idle_cycles"]
        if total_active == 0: total_active = 1

        busy_pct = (d["busy_cycles"] / total_active) * 100
        wait_pct = (d["memory_wait_cycles"] / total_active) * 100
        idle_pct = (d["active_idle_cycles"] / total_active) * 100

        display_name = short_core_name(core)
        print(f"{display_name:<25} {busy_pct:<10.1f} {wait_pct:<10.1f} {idle_pct:<10.1f} {d['load_l1sp']:<12} {d['load_l2sp']:<12} {d['load_dram']:<12} {d['icache_miss']:<10}")

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

if __name__ == "__main__":
    main()
