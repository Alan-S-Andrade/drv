// Integer-only scaled dot-product attention with fixed-point softmax (Q16.16).
// Q, K, V are int8_t; scores accum in int32; softmax probs in Q16.16; output O in int32.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>

// --------------------- CLI args ---------------------
struct Args {
    int T = 128;     // sequence length
    int D = 64;      // head dimension
    int seed = 123;  // RNG seed
    int tile = 64;   // tile for cache in score matmul
};

static int parse_int(const char* s, int defv) {
    if (!s) return defv;
    char* e=nullptr; long v=strtol(s,&e,10);
    return (e && *e==0) ? (int)v : defv;
}

static Args parse_args(int argc, char** argv){
    Args a;
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"--T") && i+1<argc) a.T = parse_int(argv[++i], a.T);
        else if (!strcmp(argv[i],"--D") && i+1<argc) a.D = parse_int(argv[++i], a.D);
        else if (!strcmp(argv[i],"--seed") && i+1<argc) a.seed = parse_int(argv[++i], a.seed);
        else if (!strcmp(argv[i],"--tile") && i+1<argc) a.tile = parse_int(argv[++i], a.tile);
        else if (!strcmp(argv[i],"--help")) {
            std::printf("Usage: %s [--T int] [--D int] [--seed int] [--tile int]\n", argv[0]);
            std::exit(0);
        }
    }
    return a;
}

// --------------------- RNG (deterministic, int only) ---------------------
static uint32_t splitmix32(uint32_t& x){
    uint32_t z = (x += 0x9e3779b9u);
    z ^= z >> 16; z *= 0x7feb352du;
    z ^= z >> 15; z *= 0x846ca68bu;
    z ^= z >> 16;
    return z;
}
static int8_t rand_i8(uint32_t& s, int lo, int hi){
    uint32_t r = splitmix32(s);
    int span = hi - lo + 1;
    int v = (int)(r % (uint32_t)span) + lo;
    return (int8_t)v;
}

// --------------------- Fixed-point helpers ---------------------
// Q16.16 layout
static const int FP = 16;
static const int32_t ONE = 1 << FP;
static inline int32_t fp_mul(int32_t a, int32_t b){ return (int32_t)(( (int64_t)a * (int64_t)b ) >> FP); }
static inline int32_t fp_div(int32_t a, int32_t b){ return (int32_t)( ((int64_t)a << FP) / (int64_t)b ); }
static inline int32_t fp_from_int(int32_t x){ return x << FP; }
static inline int32_t fp_clamp(int32_t x, int32_t lo, int32_t hi){ return x<lo?lo:(x>hi?hi:x); }

// Integer sqrt for positive 32-bit numbers (floor)
static uint32_t isqrt_u32(uint32_t x){
    uint32_t r = 0;
    uint32_t bit = 1u << 30; // The second-to-top bit
    while (bit > x) bit >>= 2;
    while (bit != 0) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; }
        else { r >>= 1; }
        bit >>= 2;
    }
    return r;
}

// Compute inv_sqrt_d in Q16.16: floor((1<<16) / sqrt(d)) with integer math
static int32_t inv_sqrt_q16(int d){
    if (d <= 0) return ONE; // avoid div/0
    uint32_t sd = isqrt_u32((uint32_t)d);
    if (sd == 0) sd = 1;
    int32_t inv = (int32_t)(((int64_t)ONE) / (int64_t)sd); // Q16.16
    return inv;
}

// Integer exp approximation on domain x in Q16.16, with x in [-8, 0].
// Use 4th-order taylor: exp(x) ≈ 1 + x + x^2/2 + x^3/6 + x^4/24
// All arithmetic in Q16.16; coefficients pre-scaled.
static inline int32_t exp_q16_clamped(int32_t x){
    // Clamp to [-8, 0]
    const int32_t MINX = -8 << FP;
    if (x < MINX) x = MINX;
    if (x > 0) x = 0;

    // Powers
    int32_t x1 = x;
    int32_t x2 = fp_mul(x1, x1);
    int32_t x3 = fp_mul(x2, x1);
    int32_t x4 = fp_mul(x3, x1);

    // Coefficients in Q16.16: 1, 1, 1/2, 1/6, 1/24
    const int32_t C0 = ONE;
    const int32_t C1 = ONE;
    const int32_t C2 = (ONE >> 1);            // 1/2
    const int32_t C3 = (int32_t)(ONE / 6);    // ~10922
    const int32_t C4 = (int32_t)(ONE / 24);   // ~2730

    int32_t y = 0;
    y += C0;
    y += fp_mul(C1, x1);
    y += fp_mul(C2, x2);
    y += fp_mul(C3, x3);
    y += fp_mul(C4, x4);
    if (y < 0) y = 0; // numeric safety
    return y;
}

// --------------------- Attention core ---------------------
// scores S = Q(K^T) scaled by inv_sqrt_d (Q16.16)
static void scores_matmul_scaled(const int8_t* Q, const int8_t* K, int32_t* S_q16,
                                 int T, int D, int tile, int32_t inv_sqrt)
{
    // Compute integer dot-products then scale to Q16.16 by inv_sqrt.
    // We tile over K dimension for cache locality.
    for (int i=0;i<T;i++){
        for (int j=0;j<T;j++){
            S_q16[(size_t)i*T + j] = 0;
        }
    }
    for (int kk=0; kk<D; kk+=tile){
        int kend = std::min(kk + tile, D);
        for (int i=0;i<T;i++){
            for (int j=0;j<T;j++){
                int32_t acc = 0;
                const int8_t* qi = &Q[(size_t)i*D + kk];
                const int8_t* kj = &K[(size_t)j*D + kk];
                for (int k=kk; k<kend; ++k){
                    acc += (int32_t)(*qi++) * (int32_t)(*kj++);
                }
                // accumulate scaled contribution in Q16.16
                int32_t scaled = (int32_t)(( (int64_t)acc * (int64_t)inv_sqrt ) ); // still Q16.16
                S_q16[(size_t)i*T + j] += scaled;
            }
        }
    }
}

// softmax in-place on each row of S_q16 (Q16.16 in, Q16.16 out), stable (subtract max)
static void softmax_rows_q16(int32_t* S_q16, int T){
    for (int i=0;i<T;i++){
        int32_t* row = &S_q16[(size_t)i*T];
        // find max
        int32_t m = row[0];
        for (int j=1;j<T;j++) if (row[j] > m) m = row[j];
        // exponentiate shifted values and sum
        int64_t sum = 0;
        for (int j=0;j<T;j++){
            int32_t x = row[j] - m;               // <= 0
            x = fp_clamp(x, -8<<FP, 0);           // clamp to [-8,0]
            int32_t e = exp_q16_clamped(x);       // Q16.16
            row[j] = e;
            sum += (int64_t)e;
        }
        if (sum <= 0) { // fallback (shouldn't happen)
            int32_t uni = fp_div(ONE, fp_from_int(T));
            for (int j=0;j<T;j++) row[j] = uni;
            continue;
        }
        // normalize
        for (int j=0;j<T;j++){
            // row[j] = row[j] / sum
            row[j] = (int32_t)( ((int64_t)row[j] << FP) / sum ); // Q16.16
        }
    }
}

// O = P * V  where P is softmax probs (Q16.16), V is int8, O is int32
static void apply_probs(const int32_t* P_q16, const int8_t* V, int32_t* O, int T, int D, int tile) {
    // O[i,d] = sum_j P[i,j] * V[j,d]
    for (int i=0;i<T;i++){
        for (int d=0; d<D; d++) O[(size_t)i*D + d] = 0;
        for (int jj=0; jj<T; jj+=tile){
            int jend = std::min(jj+tile, T);
            for (int d=0; d<D; d++){
                int64_t acc = 0;
                const int32_t* p = &P_q16[(size_t)i*T + jj];
                const int8_t*  v = &V[(size_t)jj*D + d];
                for (int j=jj; j<jend; ++j){
                    acc += (int64_t)(*p++) * (int64_t)(*v); // Q16.16 * int8
                    v += D;
                }
                // shift back Q16.16
                O[(size_t)i*D + d] += (int32_t)(acc >> FP);
            }
        }
    }
}

int main(int argc, char** argv){
    Args a = parse_args(argc, argv);
    const int T = a.T, D = a.D;

    if (T <= 0 || D <= 0) { std::fprintf(stderr, "Bad T/D\n"); return 1; }

    // Allocate
    std::vector<int8_t>  Q((size_t)T*D);
    std::vector<int8_t>  K((size_t)T*D);
    std::vector<int8_t>  V((size_t)T*D);
    std::vector<int32_t> S_q16((size_t)T*T); // scores (scaled) and later probs
    std::vector<int32_t> O((size_t)T*D);

    // Init Q,K,V with deterministic integers in [-127,127]
    uint32_t rng = (uint32_t)a.seed;
    for (size_t i=0;i<Q.size();++i) Q[i] = rand_i8(rng, -127, 127);
    for (size_t i=0;i<K.size();++i) K[i] = rand_i8(rng, -127, 127);
    for (size_t i=0;i<V.size();++i) V[i] = rand_i8(rng, -127, 127);

    // Compute inv_sqrt_d in Q16.16
    int32_t inv_sqrt = inv_sqrt_q16(D);

    auto t0 = std::chrono::steady_clock::now();
    // scores (Q16.16)
    scores_matmul_scaled(Q.data(), K.data(), S_q16.data(), T, D, a.tile, inv_sqrt);

    // For a deterministic signature on pre-softmax scores (optional)
    long long sum_scores = 0;
    for (size_t i=0;i<S_q16.size();++i) { sum_scores += (long long)S_q16[i]; }

    // softmax in-place: S_q16 becomes probs P_q16
    softmax_rows_q16(S_q16.data(), T);

    long long sum_probs = 0;
    for (size_t i=0;i<S_q16.size();++i) sum_probs += (long long)S_q16[i];

    // apply probs to V -> O
    apply_probs(S_q16.data(), V.data(), O.data(), T, D, a.tile);

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // Output deterministic signatures
    long long sum_out = 0;
    for (size_t i=0;i<O.size();++i) sum_out += (long long)O[i];

    std::printf("[attn] T=%d D=%d tile=%d  time_ms=%lld\n", T, D, a.tile, (long long)ms);
    std::printf("sum_scores=%lld\n", sum_scores); // Q16.16 sum (pre-softmax)
    std::printf("sum_probs=%lld\n",  sum_probs);  // sum of all P entries in Q16.16
    std::printf("sum_out=%lld\n",    sum_out);    // integer signature of O

    // Sanity: each softmax row should sum to ~1.0 in Q16.16; we can print average row sum
    // without any float:
    int64_t rowsum_acc = 0;
    for (int i=0;i<T;i++){
        int64_t rs = 0;
        for (int j=0;j<T;j++) rs += (int64_t)S_q16[(size_t)i*T + j];
        rowsum_acc += rs;
    }
    int32_t avg_row_sum_q16 = (int32_t)((rowsum_acc / T)); // still Q16.16
    std::printf("avg_row_sum_q16=%d (1<<16=%d)\n", (int)avg_row_sum_q16, (int)ONE);

    return 0;
}

