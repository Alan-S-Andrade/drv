// ptr_chase.cpp
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>

static int parse_i(const char* s, int d){ if(!s) return d; char* e=nullptr; long v=strtol(s,&e,10); return (e&&*e==0)?(int)v:d; }

static inline uint32_t mix32(uint32_t x){
    // Simple integer hash (no floats)
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16; return x;
}

int main(int argc, char** argv){
    // int N=1<<20, stride=7, start=0; // defaults
    // int T=1<<22;
    int N = 10000, stride = 7, start = 0;
    int T = 10000;

    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--N") && i+1<argc) N = parse_i(argv[++i], N);
        else if(!strcmp(argv[i],"--stride") && i+1<argc) stride = parse_i(argv[++i], stride);
        else if(!strcmp(argv[i],"--start") && i+1<argc) start = parse_i(argv[++i], start);
        else if(!strcmp(argv[i],"--T") && i+1<argc) T = parse_i(argv[++i], T);
        else if(!strcmp(argv[i],"--help")){
            std::printf("Usage: %s --N <int> --stride <int> --start <int> --T <int>\n", argv[0]);
            return 0;
        }
    }
    if(N<=0 || stride<=0 || start<0 || start>=N || T<0){ std::fprintf(stderr,"Bad args\n"); return 1; }

    std::vector<int> next(N);
    for(int i=0;i<N;i++) next[i] = (i + stride) % N;

    int cur = start;
    int64_t sum_idx = 0;
    uint64_t sum_val = 0;
    for(int t=0;t<T;t++){
        sum_idx += cur;
        sum_val += (uint64_t)mix32((uint32_t)cur);
        cur = next[cur];
    }
    std::printf("PTRCHASE N=%d stride=%d start=%d T=%d\n", N, stride, start, T);
    std::printf("final_index=%d\n", cur);
    std::printf("sum_idx=%lld\n", (long long)sum_idx);
    std::printf("sum_val=%llu\n", (unsigned long long)sum_val);
    return 0;
}

