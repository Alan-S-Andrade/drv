// bfs_grid.cpp
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <queue>
#include <algorithm>
#include <pandohammer/mmio.h> 
#include <pandohammer/cpuinfo.h>


static int parse_i(const char* s, int d){ if(!s) return d; char* e=nullptr; long v=strtol(s,&e,10); return (e&&*e==0)?(int)v:d; }
static inline int id(int r,int c,int C){ return r*C + c; }

int main(int argc, char** argv){
    // int R=1024, C=1024; // defaults
    int R = 1020;
    int C = 102;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--R") && i+1<argc) R = parse_i(argv[++i], R);
        else if(!strcmp(argv[i],"--C") && i+1<argc) C = parse_i(argv[++i], C);
        else if(!strcmp(argv[i],"--help")){ std::printf("Usage: %s --R <rows> --C <cols>\n", argv[0]); return 0; }
    }
    if(R<=0 || C<=0){ std::fprintf(stderr,"Bad R/C\n"); return 1; }
    const int N = R*C;

    // BFS from (0,0)
    std::vector<int> dist(N, -1);
    std::queue<int> q;
    int s = 0;
    dist[s]=0; q.push(s);

    const int dr[4]={-1,1,0,0};
    const int dc[4]={0,0,-1,1};

    while(!q.empty()){
        ph_print_int(cycle());
        int u = q.front(); q.pop();
        int ur = u / C, uc = u % C;
        for(int d=0; d<4; ++d){
            int vr = ur + dr[d], vc = uc + dc[d];
            if(vr>=0 && vr<R && vc>=0 && vc<C){
                int v = id(vr,vc,C);
                if(dist[v]==-1){
                    dist[v] = dist[u] + 1;
                    q.push(v);
                }
            }
        }
    }

    long long sum_dist = 0;
    int reached = 0, max_dist = 0;
    for(int i=0;i<N;i++){
        if(dist[i]>=0){ reached++; sum_dist += dist[i]; if(dist[i]>max_dist) max_dist=dist[i]; }
    }

    std::printf("BFS GRID R=%d C=%d N=%d\n", R, C, N);
    std::printf("reached=%d\n", reached);
    std::printf("max_dist=%d\n", max_dist);
    std::printf("sum_dist=%lld\n", sum_dist);
    return 0;
}

