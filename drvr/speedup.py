import matplotlib.pyplot as plt

# Data
threads = [16, 32, 64, 128, 256, 512, 1024]
times_ms = [6.27786, 5.2377, 5.26126, 5.2161, 5.4009, 4.69217, 5.38829]

# Evenly spaced x positions
x = range(len(threads))

plt.figure(figsize=(8, 4))
plt.plot(x, times_ms, marker='o')

plt.xlabel("Total Threads")
plt.ylabel("Total Execution Time (ms)")
plt.title("BFS Speedup (N = 10,000)")

# Label with actual thread counts
plt.xticks(x, [str(t) for t in threads])

plt.grid(True)
plt.tight_layout()
plt.savefig("bfs_speedup.png")

# 6.27786 ms
# 5.52377 ms
# 5.26126 ms
# 5.2161 ms
# 5.4009 ms
# 4.69217 ms
# 5.38829 ms