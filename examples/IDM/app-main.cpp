// SPDX-License-Identifier: MIT

#include <DrvAPIMain.hpp>
#include <DrvAPIThread.hpp>
#include <DrvAPIMemory.hpp>
#include <DrvAPI.hpp>

using namespace DrvAPI;

#include <cstdio>
#include <inttypes.h>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <iostream>

#include "inputs/binaryArr.h"

DRV_API_REF_CLASS_BEGIN(Vertex)
DRV_API_REF_CLASS_DATA_MEMBER(Vertex, id)
DRV_API_REF_CLASS_DATA_MEMBER(Vertex, edges)
DRV_API_REF_CLASS_DATA_MEMBER(Vertex, start)
DRV_API_REF_CLASS_DATA_MEMBER(Vertex, type)
DRV_API_REF_CLASS_END(Vertex)

DRV_API_REF_CLASS_BEGIN(Edge)
DRV_API_REF_CLASS_DATA_MEMBER(Edge, src)
DRV_API_REF_CLASS_DATA_MEMBER(Edge, dst)
DRV_API_REF_CLASS_DATA_MEMBER(Edge, type)
DRV_API_REF_CLASS_DATA_MEMBER(Edge, src_type)
DRV_API_REF_CLASS_DATA_MEMBER(Edge, dst_type)
DRV_API_REF_CLASS_DATA_MEMBER(Edge, src_glbid)
DRV_API_REF_CLASS_DATA_MEMBER(Edge, dst_glbid)
DRV_API_REF_CLASS_END(Edge)

DrvAPIGlobalDRAM<DrvAPIPointer<Vertex>> VArr;
DrvAPIGlobalDRAM<DrvAPIPointer<Edge>> EArr;
DrvAPIGlobalDRAM<size_t> VArrSz;
DrvAPIGlobalDRAM<size_t> EArrSz;

// use only use 2 due to very slow barrier 
// #define NUM_CORE 4
// #define NUM_THREAD 4

/*
 * number of sample
 */
int NUM_SAMPLE[] = {5, 3, 2, 1};
int MAX_NUM_NODE = 162; // 81
int MAX_NUM_EDGE = 256; // 162

int AppMain(int argc, char *argv[])
{
    using namespace DrvAPI;
    if (argc < 3) {
      printf("app <V.bin> <E.bin>\n");
      return -1;
    }

    if (DrvAPIThread::current()->threadId() == -1 &&
        DrvAPIThread::current()->coreId() == -1) {
        // not possible and not used, but need to initialize the core
        return -1;
    }


    /*
     * load image to DRAM (should be ignored)
     * Currently, whether an element is local or remote is logically determined
     * e.g. node 1 < LOCAL_RANGE is local; node 10000 > LOCAL_RANGE is remote
     */ 
    if (DrvAPIThread::current()->threadId() == 0 &&
        DrvAPIThread::current()->coreId() == 0) {

      DrvAPIMemoryAllocatorInit();
      VArr = DrvAPIMemoryAlloc(DrvAPIMemoryDRAM, 16 * 1024 * 1024);
      EArr = DrvAPIMemoryAlloc(DrvAPIMemoryDRAM, 64 * 1024 * 1024);

      { // load vertex array image
        std::ifstream varrin(argv[1], std::ios::binary);
        size_t num;
        varrin.read(reinterpret_cast<char *>(&num), sizeof(size_t));
        std::vector<unsigned char> buffer(num * sizeof(Vertex));
        varrin.read(reinterpret_cast<char *>(buffer.data()), num * sizeof(Vertex));

        Vertex *varr = reinterpret_cast<Vertex *>(buffer.data());
        for (size_t i = 0; i < num; i++) {
          VArr[i] = varr[i];
        }
        VArrSz = num;
      }

      { // load edge array image
        std::ifstream earrin(argv[2], std::ios::binary);
        size_t num;
        earrin.read(reinterpret_cast<char *>(&num), sizeof(size_t));
        std::vector<unsigned char> buffer(num * sizeof(Edge));
        earrin.read(reinterpret_cast<char *>(buffer.data()), num * sizeof(Edge));

        Edge *earr = reinterpret_cast<Edge *>(buffer.data());
        for (size_t i = 0; i < num; i++) {
          EArr[i] = earr[i];
        }
        EArrSz = num;
      }

      DrvAPI::atomic_add<int>(0x0, 1);
    }

    // barrier to wait until loading finishes
    while (DrvAPI::read<int>(0x0) == 0) DrvAPI::wait(1000);


    /*
     * a model of ego graph generation kernel
     */
    int NUM_CORE = DrvAPI::numPodCores();
    int NUM_THREAD = DrvAPI::myCoreThreads();
    size_t step = VArrSz / 4 / (NUM_CORE * NUM_THREAD);
    int tid = DrvAPIThread::current()->threadId() + DrvAPIThread::current()->coreId() * NUM_THREAD;
    size_t beg = step * tid;
    size_t end = step * (tid + 1);
    if (tid == NUM_CORE * NUM_THREAD - 1) end = VArrSz / 4;

    printf("%2d start\n", tid);
    
    // small data structures should be allocated in L1SP
    // but L1SP is too small
    DrvAPIPointer<uint64_t> frontier 
        = DrvAPIMemoryAlloc(DrvAPIMemoryDRAM, sizeof(uint64_t) * MAX_NUM_NODE);
    int frontier_head = 0, frontier_tail = 0;

    DrvAPIPointer<uint64_t> vertices
        = DrvAPIMemoryAlloc(DrvAPIMemoryDRAM, sizeof(uint64_t) * MAX_NUM_NODE);
    int vertices_size = 0;

    DrvAPIPointer<uint64_t> edgesSrc
        = DrvAPIMemoryAlloc(DrvAPIMemoryDRAM, sizeof(uint64_t) * MAX_NUM_EDGE);
    DrvAPIPointer<uint64_t> edgesDst
        = DrvAPIMemoryAlloc(DrvAPIMemoryDRAM, sizeof(uint64_t) * MAX_NUM_EDGE);
    int edges_size = 0;

    DrvAPIPointer<Edge> neighborhood
        = DrvAPIMemoryAlloc(DrvAPIMemoryDRAM, sizeof(Edge) * 5);
    int neighborhood_size = 0;


    for (size_t i = beg; i < end; i++) {
      frontier[frontier_tail++] = i;
      vertices[vertices_size++] = i;
      edgesSrc[edges_size] = i;
      edgesDst[edges_size] = i;
      edges_size++;

      int next_level = 1;
      int level = 0;
      while (frontier_head < frontier_tail) {
        uint64_t glbID = frontier[frontier_head];
        uint64_t V_localID = frontier_head;
        frontier_head++;
        // ![REMOTE/LOCAL]
        Vertex_ref va_ref = &VArr[glbID];
        Vertex V = {va_ref.id(), va_ref.edges(), va_ref.start(), va_ref.type()};

        bool not_last_level = level < 3;
        // Gather neighbors
        neighborhood_size = 0;
        if (not_last_level) {
          uint64_t startEL = V.start;
          uint64_t endEL = startEL + V.edges;
          uint64_t num_neighbors = V.edges;

          uint64_t edges_to_fetch = std::min<uint64_t>(NUM_SAMPLE[level], num_neighbors);

          for (unsigned int i = 0; i < edges_to_fetch; ++i) {
            size_t v = rand() % num_neighbors;
            // ![REMOTE/LOCAL]
            neighborhood[neighborhood_size++] = EArr[startEL + v];
          } 
        }

        for (int i = 0; i < neighborhood_size; i++) {
          Edge_ref n_erf = &neighborhood[i];
          uint64_t uGlbID = n_erf.dst_glbid();

          bool visited = false;
          int searched = -1;
          for (int j = 0; j < vertices_size; j++) {
            if (vertices[j] == uGlbID) {
              visited = true;
              searched = j;
              break;
            }
          }

          if (not_last_level && !visited) {
            uint64_t U_localID = vertices_size;
            vertices[vertices_size++] = uGlbID;

            edgesSrc[edges_size] = U_localID;
            edgesDst[edges_size] = U_localID;
            edges_size++;
            
            edgesSrc[edges_size] = V_localID;
            edgesDst[edges_size] = U_localID;
            edges_size++;

            edgesSrc[edges_size] = U_localID;
            edgesDst[edges_size] = V_localID;
            edges_size++;

            frontier[frontier_tail++] = uGlbID;
          } else if (not_last_level || visited) {
            uint64_t U_localID = searched;

            edgesSrc[edges_size] = V_localID;
            edgesDst[edges_size] = U_localID;
            edges_size++;

            edgesSrc[edges_size] = U_localID;
            edgesDst[edges_size] = V_localID;
            edges_size++;
          } 
        }


        if (frontier_head == next_level) {
          level++;
          next_level = frontier_tail;
        }
      }

      // ignore post processing steps
      // clear all data structures
      edges_size = 0;
      vertices_size = 0;
      frontier_head = frontier_tail = 0;
      neighborhood_size = 0;
    }

    printf("%2d done\n", tid);

    return 0;
}
declare_drv_api_main(AppMain);
