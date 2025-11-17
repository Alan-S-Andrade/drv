// leiden_single_int.c : integer-only Leiden-like community detection
// Compile: gcc -O2 -std=c99 leiden_single_int.c -o leiden_int
// Run:     ./leiden_int

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef MAXN
#define MAXN  1024
#endif
#ifndef MAXM
#define MAXM  8192
#endif

typedef struct {
    int n;
    int m2;
    int *rowptr;
    int *colind;
    int *deg;
} Graph;

static void graph_build_from_edges(Graph* G, int n, const int (*edges)[2], int edgeCount) {
    static int deg_tmp[MAXN];
    memset(deg_tmp, 0, sizeof(deg_tmp));
    for (int i = 0; i < edgeCount; i++) {
        int u = edges[i][0]-1;
        int v = edges[i][1]-1;
        if (u<0||v<0||u>=n||v>=n||u==v) continue;
        deg_tmp[u]++; deg_tmp[v]++;
    }
    G->n = n;
    G->m2 = 0;
    for (int i=0;i<n;i++) G->m2 += deg_tmp[i];
    G->rowptr = (int*)malloc((n+1)*sizeof(int));
    G->colind = (int*)malloc(G->m2*sizeof(int));
    G->deg    = (int*)malloc(n*sizeof(int));
    G->rowptr[0] = 0;
    for (int i=0;i<n;i++){
        G->deg[i]=deg_tmp[i];
        G->rowptr[i+1]=G->rowptr[i]+deg_tmp[i];
    }
    int *fill = (int*)calloc(n,sizeof(int));
    for (int i=0;i<edgeCount;i++){
        int u=edges[i][0]-1,v=edges[i][1]-1;
        if (u<0||v<0||u>=n||v>=n||u==v) continue;
        int pu = G->rowptr[u]+fill[u]++;
        int pv = G->rowptr[v]+fill[v]++;
        G->colind[pu]=v;
        G->colind[pv]=u;
    }
    free(fill);
}
static void graph_free(Graph* G){
    free(G->rowptr); free(G->colind); free(G->deg);
}

typedef struct {
    int n;
    int *comm_of;
    int ncomms;
    long m2;
    long *totw;
} Part;

static void part_init(Part* P, const Graph* G){
    P->n=G->n;
    P->m2=(long)G->m2;
    P->comm_of=(int*)malloc(P->n*sizeof(int));
    P->totw=(long*)calloc(P->n,sizeof(long));
    for(int i=0;i<P->n;i++){
        P->comm_of[i]=i;
        P->totw[i]=G->deg[i];
    }
    P->ncomms=P->n;
}
static void part_free(Part* P){
    free(P->comm_of); free(P->totw);
}

// integer modularity gain (scaled version)
static long delta_modularity_gain(const Graph* G, const Part* P,
                                  int node, int comm_from, int comm_to,
                                  long k_i, long k_i_in_to){
    long tot_to   = P->totw[comm_to];
    long tot_from = P->totw[comm_from]-k_i;
    long m2 = P->m2;

    // Scale all terms by m2*m2 to avoid divisions
    long gain = (k_i_in_to * m2)
              - (k_i * tot_to)
              + (k_i * tot_from);
    return gain;
}

static long local_moving_phase(const Graph* G, Part* P, int max_passes){
    int n=G->n;
    int *order=(int*)malloc(n*sizeof(int));
    for(int i=0;i<n;i++) order[i]=i;
    for(int i=0;i<n;i++){ int j=i+rand()%(n-i); int t=order[i];order[i]=order[j];order[j]=t; }

    long total_gain=0;
    int moved=1,pass=0;
    while(moved && pass++<max_passes){
        moved=0;
        for(int idx=0;idx<n;idx++){
            int u=order[idx];
            int c_from=P->comm_of[u];
            long k_u=G->deg[u];
            int maxNbr=G->rowptr[u+1]-G->rowptr[u];
            int commBuf[MAXN]; long wBuf[MAXN]; int ccount=0;

            for(int e=G->rowptr[u];e<G->rowptr[u+1];e++){
                int v=G->colind[e];
                int cv=P->comm_of[v];
                int found=-1;
                
                for(int t=0;t<ccount;t++){if(commBuf[t]==cv){found=t;break;}}
                if(found==-1){commBuf[ccount]=cv;wBuf[ccount]=1;ccount++;}
                else wBuf[found]++;
            }

            int best_c=c_from; long best_gain=0;
            for(int t=0;t<ccount;t++){
                int c_to=commBuf[t];
                if(c_to==c_from) continue;
                long gain=delta_modularity_gain(G,P,u,c_from,c_to,k_u,wBuf[t]);
                if(gain>best_gain){best_gain=gain;best_c=c_to;}
            }
            if(best_c!=c_from && best_gain>0){
                P->totw[c_from]-=k_u;
                P->totw[best_c]+=k_u;
                P->comm_of[u]=best_c;
                total_gain+=best_gain;
                moved=1;
            }
        }
    }
    free(order);
    return total_gain;
}

static int refinement_split(const Graph* G, Part* P){
    int n=G->n;
    int *visited=(int*)calloc(n,sizeof(int));
    int *queue=(int*)malloc(n*sizeof(int));
    int newc=0;
    for(int u=0;u<n;u++){
        if(visited[u]) continue;
        int oldc=P->comm_of[u];
        int front=0,back=0;
        queue[back++]=u; visited[u]=1;
        int thisc=newc++;
        while(front<back){
            int x=queue[front++];
            P->comm_of[x]=thisc;
            for(int e=G->rowptr[x];e<G->rowptr[x+1];e++){
                int y=G->colind[e];
                if(!visited[y] && P->comm_of[y]==oldc){
                    visited[y]=1;
                    queue[back++]=y;
                }
            }
        }
    }
    memset(P->totw,0,sizeof(long)*P->n);
    for(int i=0;i<n;i++) P->totw[P->comm_of[i]]+=G->deg[i];
    P->ncomms=newc;
    free(visited); free(queue);
    return newc;
}

static void print_partition(const Part* P,const char* title){
    printf("%s\n",title);
    int n=P->n; int *cid=(int*)malloc(n*sizeof(int));
    for(int i=0;i<n;i++) cid[i]=P->comm_of[i];
    int maxc=-1;for(int i=0;i<n;i++) if(cid[i]>maxc) maxc=cid[i];
    for(int c=0;c<=maxc;c++){
        int first=1;
        for(int i=0;i<n;i++){
            if(cid[i]==c){
                if(first){printf("  C%-3d: ",c);first=0;}
                printf("%d ",i+1);
            }
        }
        if(!first) printf("\n");
    }
    free(cid);
}

int main(void){
    srand(1);
    int edges[][2]={
        {1,2},{2,3},{3,1},
        {2,4},{3,4},
        {5,6},{6,7},{7,5},
        {6,8},{7,8},
        {9,10},{10,11},{11,9},
        {11,12}
    };
    int n=12;
    int edgeCount=sizeof(edges)/sizeof(edges[0]);
    Graph G; graph_build_from_edges(&G,n,edges,edgeCount);

    int max_local_passes=10;
    int levels=1; // keep simple, no aggregation for int version

    Part P; part_init(&P,&G);
    local_moving_phase(&G,&P,max_local_passes);
    refinement_split(&G,&P);
    print_partition(&P,"Communities:");
    part_free(&P);
    graph_free(&G);
    return 0;
}

