import csv
import sys
import collections

# Config: The columns we care about
INTERESTING_STATS = [
    "busy_cycles", 
    "memory_wait_cycles", 
    "active_idle_cycles", 
    "load_dram", 
    "load_l1sp",
    "icache_miss"
]

def main():
    stats = collections.defaultdict(lambda: collections.defaultdict(int))
    
    # 1. Read the CSV produced by SST
    try:
        with open("stats.csv", "r") as f:
            reader = csv.DictReader(f)
            for row in reader:
                # Parse component name (e.g., "core_0_2_mesh0.load_dram.hart_0")
                full_name = row["ComponentName"]
                stat_name = row["StatisticName"]
                
                # We only care about the Core components
                if "core" not in full_name: continue
                
                # Extract simple Core ID (e.g., "core_0_2")
                core_id = full_name.split("_mesh")[0] 
                
                if stat_name in INTERESTING_STATS:
                    # Sum up values (aggregating across all harts for loads/stores)
                    stats[core_id][stat_name] += int(row["Sum.u64"])

    except FileNotFoundError:
        print("Error: 'stats.csv' not found. Did you run the simulation?")
        sys.exit(1)

    # 2. Print the Clean Table
    headers = ["Core", "Busy %", "MemWait %", "Idle %", "DRAM Loads", "L1 Hits", "I-Miss"]
    print(f"{headers[0]:<15} {headers[1]:<10} {headers[2]:<10} {headers[3]:<10} {headers[4]:<12} {headers[5]:<10} {headers[6]:<10}")
    print("-" * 80)

    for core in sorted(stats.keys()):
        d = stats[core]
        
        # Calculate percentages
        total_active = d["busy_cycles"] + d["memory_wait_cycles"] + d["active_idle_cycles"]
        if total_active == 0: total_active = 1 # avoid div/0
        
        busy_pct = (d["busy_cycles"] / total_active) * 100
        wait_pct = (d["memory_wait_cycles"] / total_active) * 100
        idle_pct = (d["active_idle_cycles"] / total_active) * 100
        
        print(f"{core:<15} {busy_pct:<10.1f} {wait_pct:<10.1f} {idle_pct:<10.1f} {d['load_dram']:<12} {d['load_l1sp']:<10} {d['icache_miss']:<10}")

if __name__ == "__main__":
    main()
