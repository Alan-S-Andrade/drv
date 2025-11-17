#include <stdio.h>
#include <DrvAPI.hpp>
static auto _drvapi_ref = DrvAPIGetCurrentContext();

#define maximum_nodes 12

int SimpleMain(int argc, char *argv[]) {
    // Triangles: (1,2,3), (2,3,4), (5,6,7), (6,7,8), (9,10,11)
    // Edges listed below (undirected)

    // int edges[][2] = {
    //     {1,2}, {2,3}, {3,1},   // triangle 1
    //     {2,4}, {3,4},          // triangle 2 shares nodes 2,3
    //     {5,6}, {6,7}, {7,5},   // triangle 3
    //     {6,8}, {7,8},          // triangle 4 shares nodes 6,7
    //     {9,10}, {10,11}, {11,9}, // triangle 5
    //     {11,12}                 // extra edge (no triangle)
    // };


    int edges[][2] = {
        {1,2}, {2,3}, {3,1},   // triangle 1
        {2,4}, {3,4},          // triangle 2 shares nodes 2,3
        {5,6}, {6,7}, {7,5},   // triangle 3
        {6,8}, {7,8},          // triangle 4 shares nodes 6,7
        {9,10}, {10,11}, {11,9}, // triangle 5
        {11,12},
        {30,1412},
    {1412,30},
    {30,3352},
    {3352,30},
    {30,5254},
    {5254,30},
    {30,5543},
    {5543,30},
    {30,7478},
    {7478,30},
    {3,28}           // extra edge (no triangle)
};
    int edgeCount = sizeof(edges) / sizeof(edges[0]);

    int adj[maximum_nodes + 1][maximum_nodes + 1] = {0};

    // Build adjacency matrix
    for (int i = 0; i < edgeCount; i++) {
        int u = edges[i][0];
        int v = edges[i][1];
        adj[u][v] = 1;
        adj[v][u] = 1;
    }

    int triangles = 0;

    // Count triangles (u < v < w)
    for (int u = 1; u <= maximum_nodes; u++) {
        for (int v = u + 1; v <= maximum_nodes; v++) {
            if (!adj[u][v]) continue;
            for (int w = v + 1; w <= maximum_nodes; w++) {
                if (adj[u][w] && adj[v][w]) {
                    triangles++;
                    printf("Triangle found: (%d, %d, %d)\n", u, v, w);
                }
            }
        }
    }

    printf("\the total triangles found: %d\n", triangles);
    return 0;
}

declare_drv_api_main(SimpleMain);