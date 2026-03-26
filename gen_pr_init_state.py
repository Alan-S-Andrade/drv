#!/usr/bin/env python3
"""Pre-generate initialized PageRank data structures for ROI-only simulation.

Reads uniform_graph.bin header and degree array, then generates:
  - pr_rank_init.bin:    N int32 values, each = 1000000 // N
  - pr_contrib_init.bin: N int32 values, each = (1000000 // N) // degree[v]
                         (0 for vertices with degree 0)

Format: raw little-endian int32 arrays, no header.
"""
import argparse
import struct
import array


def main():
    parser = argparse.ArgumentParser(
        description="Generate pre-initialized PageRank state for ROI-only simulation")
    parser.add_argument("--graph", required=True,
                        help="Path to uniform_graph.bin")
    parser.add_argument("-o", "--output-dir", required=True,
                        help="Output directory for generated files")
    args = parser.parse_args()

    # Read graph header: 5 x int32 (N, E, avg_degree, unused, source)
    with open(args.graph, "rb") as f:
        header = struct.unpack("<5i", f.read(20))
        N = header[0]
        E = header[1]

        # Read offsets array to compute degree, or read degree array at end
        # Degree array is at offset: 20 + (N+1)*4 + E*4
        degree_offset = 20 + (N + 1) * 4 + E * 4
        f.seek(degree_offset)
        degree_data = f.read(N * 4)

    degree = array.array("i")
    degree.frombytes(degree_data)

    RANK_SCALE = 1000000
    init_rank = RANK_SCALE // N

    print(f"Graph: N={N}, E={E}")
    print(f"RANK_SCALE={RANK_SCALE}, init_rank={init_rank}")

    # Generate rank[] init: all = init_rank
    rank = array.array("i", [init_rank] * N)

    rank_path = f"{args.output_dir}/pr_rank_init.bin"
    with open(rank_path, "wb") as f:
        rank.tofile(f)
    print(f"Wrote {rank_path}: {N * 4} bytes")

    # Generate contrib[] init: contrib[v] = init_rank / degree[v]
    contrib = array.array("i", [0] * N)
    for v in range(N):
        d = degree[v]
        if d > 0:
            contrib[v] = init_rank // d
        else:
            contrib[v] = 0

    contrib_path = f"{args.output_dir}/pr_contrib_init.bin"
    with open(contrib_path, "wb") as f:
        contrib.tofile(f)
    print(f"Wrote {contrib_path}: {N * 4} bytes")


if __name__ == "__main__":
    main()
