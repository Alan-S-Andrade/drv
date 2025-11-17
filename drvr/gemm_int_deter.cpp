// gemm_int.cpp
// Single-file integer-only GEMM: C = A (MxK) * B (KxN)
// A, B are int8_t; accumulate and output in int32_t.
// No floating point is used anywhere.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>
#include <string>
#include <chrono>
#include <algorithm>

struct Args {
    int M = 256;
    int N = 256;
    int K = 256;
    int seed = 123;
    int check = 1;        // 1 = verify against naive reference
    int tile_m = 128;     // tile sizes (can be tuned)
    int tile_n = 128;
    int tile_k = 128;
};

static int parse_int(const char* s, int defv) {
    if (!s) return defv;
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    return (end && *end == 0) ? (int)v : defv;
}

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--M") == 0 && i+1 < argc) a.M = parse_int(argv[++i], a.M);
        else if (std::strcmp(argv[i], "--N") == 0 && i+1 < argc) a.N = parse_int(argv[++i], a.N);
        else if (std::strcmp(argv[i], "--K") == 0 && i+1 < argc) a.K = parse_int(argv[++i], a.K);
        else if (std::strcmp(argv[i], "--seed") == 0 && i+1 < argc) a.seed = parse_int(argv[++i], a.seed);
        else if (std::strcmp(argv[i], "--check") == 0 && i+1 < argc) a.check = parse_int(argv[++i], a.check);
        else if (std::strcmp(argv[i], "--tm") == 0 && i+1 < argc) a.tile_m = parse_int(argv[++i], a.tile_m);
        else if (std::strcmp(argv[i], "--tn") == 0 && i+1 < argc) a.tile_n = parse_int(argv[++i], a.tile_n);
        else if (std::strcmp(argv[i], "--tk") == 0 && i+1 < argc) a.tile_k = parse_int(argv[++i], a.tile_k);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf(
                "Usage: %s [--M m] [--N n] [--K k] [--seed s] [--check 0|1] [--tm Tm] [--tn Tn] [--tk Tk]\n",
                argv[0]);
            std::exit(0);
        }
    }
    return a;
}

// Row-major index helpers
static inline size_t idx2(size_t r, size_t c, size_t stride) { return r*stride + c; }

// Reference GEMM (naive, integer-only)
static void gemm_ref(const int8_t* A, const int8_t* B, int32_t* C, int M, int N, int K) {
    for (int i = 0; i < M; ++i) {
        for (int j = 0; j < N; ++j) {
            int32_t acc = 0;
            for (int k = 0; k < K; ++k) {
                // Promote to int32 for the multiply-accumulate
                acc += (int32_t)A[idx2(i, k, K)] * (int32_t)B[idx2(k, j, N)];
            }
            C[idx2(i, j, N)] = acc;
        }
    }
}

// Tiled GEMM (cache-friendly), integer-only
static void gemm_tiled(const int8_t* A, const int8_t* B, int32_t* C,
                       int M, int N, int K, int TM, int TN, int TK)
{
    // Initialize C to zero
    std::fill(C, C + (size_t)M * (size_t)N, 0);

    for (int i0 = 0; i0 < M; i0 += TM) {
        int i_max = std::min(i0 + TM, M);
        for (int k0 = 0; k0 < K; k0 += TK) {
            int k_max = std::min(k0 + TK, K);
            for (int j0 = 0; j0 < N; j0 += TN) {
                int j_max = std::min(j0 + TN, N);

                // Compute block C[i0:i_max, j0:j_max] += A[i0:i_max, k0:k_max] * B[k0:k_max, j0:j_max]
                for (int i = i0; i < i_max; ++i) {
                    for (int k = k0; k < k_max; ++k) {
                        int32_t a_ik = (int32_t)A[idx2(i, k, K)];
                        const int8_t* b_row = &B[idx2(k, j0, N)];
                        int32_t* c_row = &C[idx2(i, j0, N)];
                        // Unroll small inner j loop lightly for throughput
                        int j = j0;
                        for (; j + 4 <= j_max; j += 4) {
                            c_row[j - j0 + 0] += a_ik * (int32_t)b_row[0];
                            c_row[j - j0 + 1] += a_ik * (int32_t)b_row[1];
                            c_row[j - j0 + 2] += a_ik * (int32_t)b_row[2];
                            c_row[j - j0 + 3] += a_ik * (int32_t)b_row[3];
                            b_row += 4;
                        }
                        for (; j < j_max; ++j) {
                            c_row[j - j0] += a_ik * (int32_t)(*b_row++);
                        }
                    }
                }
            }
        }
    }
}

// Simple integer RNG in range [lo, hi]
static int8_t rand_i8(std::mt19937& rng, int lo, int hi) {
    // (hi - lo + 1) assumed <= 256
    uint32_t r = rng();
    int v = (int)(r % (uint32_t)(hi - lo + 1)) + lo;
    return (int8_t)v;
}

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    const int M = args.M, N = args.N, K = args.K;

    // Allocate
    std::vector<int8_t>  A((size_t)M*(size_t)K);
    std::vector<int8_t>  B((size_t)K*(size_t)N);
    std::vector<int32_t> C((size_t)M*(size_t)N);
    std::vector<int32_t> Cref;

    if (args.check) {
        Cref.resize((size_t)M*(size_t)N);
    }

    // Init A, B with deterministic integer data in [-127, 127]
    std::mt19937 rng((uint32_t)args.seed);
    FILE* fa = fopen("A.bin", "rb");
    FILE* fb = fopen("B.bin", "rb");
    fread(A.data(), sizeof(int8_t), A.size(), fa);
    fread(B.data(), sizeof(int8_t), B.size(), fb);
    fclose(fa);
    fclose(fb);

    // Optional reference
    if (args.check) {
        auto t0 = std::chrono::steady_clock::now();
        gemm_ref(A.data(), B.data(), Cref.data(), M, N, K);
        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::printf("[ref] M=%d N=%d K=%d  time_ms=%lld\n", M, N, K, (long long)ms);
    }

    // Tiled GEMM
    auto t2 = std::chrono::steady_clock::now();
    gemm_tiled(A.data(), B.data(), C.data(), M, N, K, args.tile_m, args.tile_n, args.tile_k);
    auto t3 = std::chrono::steady_clock::now();
    auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
    std::printf("[opt] M=%d N=%d K=%d  tm=%d tn=%d tk=%d  time_ms=%lld\n",
                M, N, K, args.tile_m, args.tile_n, args.tile_k, (long long)ms2);

    // Verify (integer compare)
    if (args.check) {
        size_t mismatches = 0;
        for (size_t i = 0; i < C.size(); ++i) {
            if (C[i] != Cref[i]) {
                if (mismatches < 10) {
                    std::printf("Mismatch at idx %zu: got %d, ref %d\n",
                                i, (int)C[i], (int)Cref[i]);
                }
                ++mismatches;
            }
        }
        if (mismatches == 0) {
            std::printf("VERIFY: OK\n");
        } else {
            std::printf("VERIFY: FAIL  mismatches=%zu\n", mismatches);
            return 1;
        }
    }

    // Print a simple checksum so you can sanity-check outputs deterministically without floats
    // Checksum is sum of C elements modulo 2^31-1 (still integer).
    int64_t checksum = 0;
    for (size_t i = 0; i < C.size(); ++i) {
        checksum += (int64_t)C[i];
        // keep it from overflowing 64-bit during enormous runs by modular reduction
        const int64_t MOD = 2147483647; // prime, fits in 31 bits
        if (checksum > (MOD << 2)) checksum %= MOD;
    }
    std::printf("CHECKSUM: %lld\n", (long long)(checksum));

    return 0;
}
