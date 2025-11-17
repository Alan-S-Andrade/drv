# convert_adjlist_to_c_edges.py

def convert_to_c_edges(input_file, output_file=None):
    edges = []
    with open(input_file, 'r') as f:
        for line in f:
            if not line.strip():
                continue
            parts = line.strip().split()
            if len(parts) == 2:
                u, v = parts
                edges.append((int(u), int(v)))

    # Generate C-style output
    output = []
    output.append("int edges[][2] = {")
    for i, (u, v) in enumerate(edges):
        comma = "," if i != len(edges) - 1 else ""
        output.append(f"    {{{u},{v}}}{comma}")
    output.append("};")
    output.append("int edgeCount = sizeof(edges) / sizeof(edges[0]);")
    output.append("")
    output.append("int adj[maximum_nodes + 1][maximum_nodes + 1] = {0};")
    output.append("")
    output.append("// Build adjacency matrix")
    output.append("for (int i = 0; i < edgeCount; i++) {")
    output.append("    int u = edges[i][0];")
    output.append("    int v = edges[i][1];")
    output.append("    adj[u][v] = 1;")
    output.append("    adj[v][u] = 1;")
    output.append("}")

    formatted_output = "\n".join(output)
    if output_file:
        with open(output_file, 'w') as f:
            f.write(formatted_output)
        print(f"✅ Output written to {output_file}")
    else:
        print(formatted_output)


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Convert adjacency list to C-style edge array")
    parser.add_argument("input_file", help="Path to the adjacency list file")
    parser.add_argument("-o", "--output_file", help="Optional output file path")
    args = parser.parse_args()

    convert_to_c_edges(args.input_file, args.output_file)

