// list_traverse.cpp
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int parse_i(const char* s, int d) {
    if(!s) {
        return d;
    }

    char* e=nullptr;
    long v=strtol(s,&e,10);
    return (e&&*e==0)?(int)v:d;
}

static inline uint32_t mix32(uint32_t x){
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16; return x;
}

struct Node {
    int next;
    uint32_t val; 
};

int main(int argc, char** argv) {
    // int N = 1<<22; // default 4M nodes
    int N = 10000;
    for (int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--N") && i+1<argc) N = parse_i(argv[++i], N);
        else if(!strcmp(argv[i],"--help")){ std::printf("Usage: %s --N <int>\n", argv[0]); return 0; }
    }

    if (N<=0) {
        std::fprintf(stderr,"Bad N\n");
        return 1; 
    }

    std::vector<Node> pool(N);
    for(int i=0;i<N;i++){
        pool[i].next = (i+1<N)?(i+1):-1;
        pool[i].val  = mix32((uint32_t)i);
    }

    int cur = 0, count=0;
    uint64_t sum_val = 0;
    uint32_t xor_ids = 0;
    while(cur!=-1){
        sum_val += pool[cur].val;
        xor_ids ^= (uint32_t)cur;
        cur = pool[cur].next;
        count++;
    }

    std::printf("LIST N=%d\n", N);
    std::printf("visited=%d\n", count);
    std::printf("sum_val=%llu\n", (unsigned long long)sum_val);
    std::printf("xor_ids=%u\n", xor_ids);
    return 0;
}

