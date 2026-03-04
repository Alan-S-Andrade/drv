#!/usr/bin/env python3
"""Generate a uniform-degree random CSR graph in binary format.

Binary format (all int32 little-endian):
  header:  N, num_edges, degree, 0 (unused), source
  offsets: (N+1) values  -- CSR row pointers
  edges:   num_edges values -- CSR column indices (random neighbors)
  degrees: N values -- per-vertex degree (all equal for uniform graphs)
"""
import argparse
import struct
import random
import array


def main():
    parser = argparse.ArgumentParser(description="Generate uniform-degree CSR graph")
    parser.add_argument("-N", type=int, required=True, help="Number of vertices")
    parser.add_argument("-D", type=int, required=True, help="Degree per vertex")
    parser.add_argument("--seed", type=int, default=42, help="RNG seed")
    parser.add_argument("-o", "--output", required=True, help="Output binary file path")
    parser.add_argument("--source", type=int, default=0, help="BFS source vertex")
    args = parser.parse_args()

    N = args.N
    D = args.D
    E = N * D
    source = args.source
    rng = random.Random(args.seed)

    with open(args.output, "wb") as f:
        # Header: 5 x int32
        f.write(struct.pack("<5i", N, E, D, 0, source))

        # CSR offsets: uniform degree so offsets = [0, D, 2D, ..., N*D]
        offsets = array.array("i", (i * D for i in range(N + 1)))
        offsets.tofile(f)

        # CSR edges: random neighbors in [0, N) for each vertex
        # Write in chunks to avoid huge memory allocation for large graphs
        CHUNK = 1 << 20  # ~1M edges per chunk
        remaining = E
        while remaining > 0:
            n = min(CHUNK, remaining)
            edges = array.array("i", (rng.randrange(N) for _ in range(n)))
            edges.tofile(f)
            remaining -= n

        # Per-vertex degree array (all D for uniform)
        deg = struct.pack("<i", D)
        for _ in range(N):
            f.write(deg)

    file_size = 20 + (N + 1) * 4 + E * 4 + N * 4
    print("Generated {}: N={} E={} D={} source={} ({} bytes)".format(
        args.output, N, E, D, source, file_size))


if __name__ == "__main__":
    main()
