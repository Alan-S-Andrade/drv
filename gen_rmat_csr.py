#!/usr/bin/env python3
"""Generate a Graph500-style RMAT power-law CSR graph in binary format.

Uses Kronecker product with parameters a=0.57, b=0.19, c=0.19, d=0.05.
Produces an undirected graph (both directions stored), self-loops removed,
duplicates removed.

Binary format (same as gen_uniform_csr.py, all int32 little-endian):
  header:  N, num_edges, avg_degree, 0 (unused), source
  offsets: (N+1) values  -- CSR row pointers
  edges:   num_edges values -- CSR column indices
  degrees: N values -- per-vertex out-degree
"""
import argparse
import struct
import array
import random


def generate_rmat_edges(scale, edgefactor, a=0.57, b=0.19, c=0.19, seed=42):
    """Generate RMAT Kronecker edge list (undirected, no self-loops, no dups)."""
    N = 1 << scale
    M = edgefactor * N  # directed edges to generate

    rng = random.Random(seed)
    ab = a + b
    abc = a + b + c

    edges = set()
    for _ in range(M):
        u, v = 0, 0
        for bit in range(scale - 1, -1, -1):
            r = rng.random()
            if r < a:
                pass                  # (0,0) quadrant
            elif r < ab:
                v |= (1 << bit)       # (0,1) quadrant
            elif r < abc:
                u |= (1 << bit)       # (1,0) quadrant
            else:
                u |= (1 << bit)       # (1,1) quadrant
                v |= (1 << bit)

        if u != v:
            edges.add((u, v))
            edges.add((v, u))  # undirected

    return N, sorted(edges)


def edges_to_csr(N, edge_list):
    """Convert sorted edge list to CSR arrays."""
    degrees = [0] * N
    for u, _ in edge_list:
        degrees[u] += 1

    offsets = [0] * (N + 1)
    for i in range(N):
        offsets[i + 1] = offsets[i] + degrees[i]

    edges = [0] * len(edge_list)
    pos = list(offsets)  # copy
    for u, v in edge_list:
        edges[pos[u]] = v
        pos[u] += 1

    return offsets, edges, degrees


def main():
    parser = argparse.ArgumentParser(description="Generate RMAT power-law CSR graph")
    parser.add_argument("--scale", type=int, required=True,
                        help="Graph scale (N = 2^scale)")
    parser.add_argument("--ef", type=int, default=16,
                        help="Edge factor (edges generated = ef * N)")
    parser.add_argument("--seed", type=int, default=42, help="RNG seed")
    parser.add_argument("-o", "--output", required=True, help="Output binary file path")
    parser.add_argument("--source", type=int, default=-1,
                        help="BFS source vertex (-1 = max degree vertex)")
    args = parser.parse_args()

    scale = args.scale
    ef = args.ef

    print(f"Generating RMAT graph: scale={scale} (N={1 << scale}), ef={ef}...")
    N, edge_list = generate_rmat_edges(scale, ef, seed=args.seed)
    E = len(edge_list)
    print(f"  Edges after dedup+undirected: {E}")

    offsets, edges, degrees = edges_to_csr(N, edge_list)

    # Pick source: max-degree vertex by default
    if args.source >= 0:
        source = args.source
    else:
        source = max(range(N), key=lambda v: degrees[v])

    avg_degree = E // N if N > 0 else 0
    max_deg = max(degrees) if degrees else 0
    min_deg = min(degrees) if degrees else 0

    print(f"  Degree: avg={avg_degree}, max={max_deg}, min={min_deg}")
    print(f"  Source vertex: {source} (degree={degrees[source]})")

    with open(args.output, "wb") as f:
        # Header: 5 x int32
        f.write(struct.pack("<5i", N, E, avg_degree, 0, source))

        # CSR offsets
        arr = array.array("i", offsets)
        arr.tofile(f)

        # CSR edges
        arr = array.array("i", edges)
        arr.tofile(f)

        # Per-vertex degrees
        arr = array.array("i", degrees)
        arr.tofile(f)

    file_size = 20 + (N + 1) * 4 + E * 4 + N * 4
    print(f"Generated {args.output}: N={N} E={E} avg_deg={avg_degree} "
          f"source={source} ({file_size} bytes)")


if __name__ == "__main__":
    main()
