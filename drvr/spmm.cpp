// Copyright (c) 2026 The University of Texas at Austin
// SPDX-License-Identifier: MIT

// Real multihart SpMM (CSR * dense features) in the same “all harts run + barrier” style
// as your BFS/PageRank.
//
// Key change vs your prototype:
//   - Your code *simulates* 3 threads by calling MergePathSpMM() three times on hart0.
//   - A real multihart version needs *all harts* to participate concurrently.
//   - Also, your current MergePath partition can cause two threads to update the same output row
//     at boundaries, which would require atomic float adds (not ideal / may not exist).
//
// This version avoids that problem entirely by giving each hart **exclusive ownership of a disjoint
// range of rows**. Then each output[row][col] is written by exactly one hart → no atomics needed.
//
// If you still want true MergePath partitioning across (rows + nnz) for load balance, we can do it,
// but you’ll need either atomic float adds or a two-phase reduction scheme.

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>

#include <pandohammer/cpuinfo.h>
#include <pandohammer/mmio.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>

static constexpr int HARTS = 16;

static int64_t thread_phase_counter[HARTS];
static volatile int64_t global_barrier_count = 0;
static volatile int64_t global_barrier_phase = 0;

static inline void barrier(int total_harts) {
    uint64_t hid = (myCoreId() << 4) + myThreadId();
    const int64_t cur = thread_phase_counter[hid];

    const int64_t old = atomic_fetch_add_i64(&global_barrier_count, 1);
    if (old == total_harts - 1) {
        atomic_swap_i64(&global_barrier_count, 0);
        atomic_fetch_add_i64(&global_barrier_phase, 1);
    } else {
        long w = 1;
        long wmax = 8 * 1024;
        while (global_barrier_phase == cur) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }
    thread_phase_counter[hid] = cur + 1;
}

class CSRMatrix {
public:
    std::vector<float> values;     // nnz
    std::vector<int>   colIndices; // nnz
    std::vector<int>   rowOffsets; // numRows+1
    int numRows = 0;

    CSRMatrix(int num_rows,
              int num_edges,
              std::vector<int>& coo_row_indices,
              std::vector<int>& coo_col_indices)
        : numRows(num_rows)
    {
        values.assign(num_edges, 1.0f);
        colIndices.resize(num_edges);

        // sort (row,col) by row then col (good for CSR + sorted neighbors)
        std::vector<std::tuple<int,int>> t;
        t.reserve(coo_row_indices.size());
        for (size_t i = 0; i < coo_row_indices.size(); i++) {
            t.emplace_back(coo_row_indices[i], coo_col_indices[i]);
        }
        std::sort(t.begin(), t.end(),
                  [](const auto& a, const auto& b) {
                      if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) < std::get<0>(b);
                      return std::get<1>(a) < std::get<1>(b);
                  });

        for (size_t i = 0; i < t.size(); i++) {
            coo_row_indices[i] = std::get<0>(t[i]);
            coo_col_indices[i] = std::get<1>(t[i]);
        }
        colIndices = std::move(coo_col_indices);

        // Build rowOffsets of size numRows+1
        rowOffsets.assign(numRows + 1, 0);
        for (int idx : coo_row_indices) {
            // assumes 0-based rows in file; if 1-based, subtract 1 when reading
            assert(idx >= 0 && idx < numRows);
            rowOffsets[idx + 1]++;
        }
        // prefix sum
        for (int r = 0; r < numRows; r++) rowOffsets[r + 1] += rowOffsets[r];

        // NOTE: values[] is already 1.0, and colIndices already in row-major order,
        // so we’re done. (If you needed stable placement from unsorted COO, you’d scatter.)
    }
};

static int readMtx(const char* fname, int& num_rows, int& num_cols, int& num_nzs, std::vector<int>& row_indices, std::vector<int>& col_indices) {
    FILE* fp = fopen(fname, "r");
    std::printf("Reading file '%s'\n", fname);
    if (!fp) {
        std::printf("Error: failed to open '%s'\n", fname);
        return -1;
    }

    if (fscanf(fp, "%d %d %d", &num_rows, &num_cols, &num_nzs) != 3) {
        std::printf("Error: bad header in '%s'\n", fname);
        fclose(fp);
        return -1;
    }

    row_indices.resize(num_nzs);
    col_indices.resize(num_nzs);

    for (int i = 0; i < num_nzs; i++) {
        int src, dst;
        if (fscanf(fp, "%d %d", &src, &dst) != 2) {
            std::printf("Error: unexpected EOF in '%s'\n", fname);
            fclose(fp);
            return -1;
        }

        // IMPORTANT: if your file is 1-based, uncomment these:
        // src--; dst--;

        row_indices[i] = src;
        col_indices[i] = dst;
    }

    fclose(fp);
    return 0;
}

static int readFeaturesFlat(const char* fname, int& feat_rows, int& feat_cols,
                            std::vector<float>& feat) {
    FILE* fp = fopen(fname, "r");
    std::printf("Reading file '%s'\n", fname);
    if (!fp) return -1;

    if (fscanf(fp, "%d %d", &feat_rows, &feat_cols) != 2) {
        fclose(fp);
        return -1;
    }

    feat.resize((size_t)feat_rows * (size_t)feat_cols);

    for (int i = 0; i < feat_rows; i++) {
        for (int j = 0; j < feat_cols; j++) {
            float v;
            if (fscanf(fp, "%f", &v) != 1) {
                fclose(fp);
                return -1;
            }
            feat[(size_t)i * feat_cols + j] = v;
        }
    }

    fclose(fp);
    return 0;
}

// -------------------- Multihart SpMM --------------------
// output[row, col] = sum_{offset in row} values[offset] * features[colIndices[offset], col]
static void SpMM_CSR_rowsliced_parallel(int total_harts, CSRMatrix& graph,
                                       const std::vector<float>& features, // flat [numCols x featDim]?? actually [node][featDim]
                                       int feat_rows,
                                       int feat_cols,
                                       std::vector<float>& output) {  // flat [numRows][feat_cols]
    const uint64_t hid = (myCoreId() << 4) + myThreadId();

    // Partition rows
    const int numRows = graph.numRows;
    const int begin = (numRows * hid) / total_harts;
    const int end   = (numRows * (hid + 1)) / total_harts;

    for (int row = begin; row < end; row++) {
        const int start = graph.rowOffsets[row];
        const int stop  = graph.rowOffsets[row + 1];

        // For each feature dimension
        for (int col = 0; col < feat_cols; col++) {
            float acc = 0.0f;

            for (int off = start; off < stop; off++) {
                const int nbr = graph.colIndices[off];
                // bounds: nbr should be in [0, feat_rows)
                acc += graph.values[off] * features[(size_t)nbr * feat_cols + col];
            }

            output[(size_t)row * feat_cols + col] = acc;
        }
    }
}

int main() {
    // Use only threadId within a core, assuming HARTS harts participating.
    // If you have multiple cores with harts each, you’ll want a global hart id and a global barrier.
    const uint64_t hid = (myCoreId() << 4) + myThreadId();

    if (hid == 0) {
        for (int i = 0; i < HARTS; i++) thread_phase_counter[i] = 0;
        printf("SpMM (real multihart, row-sliced)\n");
    }

    // Shared data, initialized by hart0
    static CSRMatrix* graph_ptr = nullptr;
    static int numRows = 0, numCols = 0, numNzs = 0;
    static int feat_rows = 0, feat_cols = 0;
    static std::vector<float> features; // flat
    static std::vector<float> output;   // flat

    if (hid == 0) {
        printf("Reading the graph\n");
        std::vector<int> rowIdx, colIdx;
        if (readMtx("spmm.graph.mtx", numRows, numCols, numNzs, rowIdx, colIdx) != 0) {
            printf("readMtx failed\n");
            std::exit(1);
        }

        printf("Reading the features\n");
        if (readFeaturesFlat("spmm.features", feat_rows, feat_cols, features) != 0) {
            printf("readFeatures failed\n");
            std::exit(1);
        }

        // Basic sanity checks
        if (feat_rows < numCols) {
            // If the graph columns index nodes, feat_rows must cover them.
            std::printf("ERROR: features rows (%d) < graph numCols (%d)\n", feat_rows, numCols);
            std::exit(1);
        }

        printf("Constructing CSR\n");
        graph_ptr = new CSRMatrix(numRows, numNzs, rowIdx, colIdx);

        printf("CSR: numRows=%d nnz=%d feat_cols=%d\n", numRows, numNzs, feat_cols);

        // Allocate output [numRows][feat_cols], zero init
        output.assign((size_t)numRows * (size_t)feat_cols, 0.0f);
    }

    barrier(HARTS);

    // All harts participate in compute
    SpMM_CSR_rowsliced_parallel(HARTS, *graph_ptr, features, feat_rows, feat_cols, output);

    barrier(HARTS);

    if (hid == 0) {
        // Print a tiny checksum-ish sample to show it ran
        ph_puts("Done. Sample output:\n");
        const int sample_rows = std::min(numRows, 4);
        const int sample_cols = std::min(feat_cols, 4);
        for (int r = 0; r < sample_rows; r++) {
            for (int c = 0; c < sample_cols; c++) {
                float v = output[(size_t)r * feat_cols + c];
                ph_print_float(v);
            }
        }

        delete graph_ptr;
        graph_ptr = nullptr;
    }

    barrier(HARTS);
    return 0;
}
