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

inline Vertex readVertexRef(const Vertex_ref &r) {
  return {r.id(), r.edges(), r.start(), r.type()};
}

DRV_API_REF_CLASS_BEGIN(Edge)
DRV_API_REF_CLASS_DATA_MEMBER(Edge, src)
DRV_API_REF_CLASS_DATA_MEMBER(Edge, dst)
DRV_API_REF_CLASS_DATA_MEMBER(Edge, type)
DRV_API_REF_CLASS_DATA_MEMBER(Edge, src_type)
DRV_API_REF_CLASS_DATA_MEMBER(Edge, dst_type)
DRV_API_REF_CLASS_DATA_MEMBER(Edge, src_glbid)
DRV_API_REF_CLASS_DATA_MEMBER(Edge, dst_glbid)
DRV_API_REF_CLASS_END(Edge)

inline Edge readEdgeRef(const Edge_ref &r) {
  return {r.src(), r.dst(), r.type(), r.src_type(), r.dst_type(), r.src_glbid(), r.dst_glbid()};
}

#define DRV_READ_STRUCT_DEF(type)                       \
  inline type read ## type (const DrvAPIPointer< type > &p, size_t pos) { \
    type ## _ref r = &p[pos];                                             \
    return read ## type ## Ref(r);                                        \
  }

DRV_READ_STRUCT_DEF(Vertex)
DRV_READ_STRUCT_DEF(Edge)

struct Data01CSR {
  DrvAPIPointer<Vertex> VArr;
  DrvAPIPointer<Edge> EArr;

  size_t VSize = 0, ESize = 0;

  Data01CSR(DrvAPIAddress addr) {
    // uint64_t imageAddr = DrvAPIVAddress::MainMemBase(0).encode() + 0x38000000;
    uint64_t imageAddr = addr;
    uint64_t VArrAddr = imageAddr;
    uint64_t EArrAddr = VArrAddr + 6349960;

    VSize = DrvAPI::read<uint64_t>(VArrAddr);
    ESize = DrvAPI::read<uint64_t>(EArrAddr);
    VArr = DrvAPIPointer<Vertex>(VArrAddr + 8);
    EArr = DrvAPIPointer<Edge>(EArrAddr + 8);
  }
};

struct CSRInterface {
  Data01CSR local, remote;
  uint64_t VArrSz, EArrSz;
  uint64_t VLocalAccessCnt = 0, VRemoteAccessCnt = 0;
  uint64_t ELocalAccessCnt = 0, ERemoteAccessCnt = 0;

  CSRInterface(unsigned lpxn, unsigned rpxn):
    local(DrvAPIVAddress::MainMemBase(lpxn).encode() + 0x38000000),
    remote(DrvAPIVAddress::MainMemBase(rpxn).encode() + 0x38000000) 
  {
    VArrSz = local.VSize;
    EArrSz = local.ESize;
  }

  bool localVertexPos(size_t n) { return n < VArrSz / 2;}
  bool localEdgePos(size_t n) { return n < EArrSz / 2;}

  Vertex V(size_t n) {
    if (localVertexPos(n)) {
      VLocalAccessCnt++;
      return readVertex(local.VArr, n);
    } else {
      VRemoteAccessCnt++;
      return readVertex(remote.VArr, n);
    }
  }

  Edge E(size_t n) {
    if (localEdgePos(n)) {
      ELocalAccessCnt++;
      return readEdge(local.EArr, n);
    } else {
      ERemoteAccessCnt++;
      return readEdge(remote.EArr, n);
    }
  }
};

DrvAPIGlobalDRAM<int> g_barrier;
int totalThreads() {
  return myCoreThreads() * numPodCores() * numPXNPods();
}

/*
 * number of sample
 */
int NUM_SAMPLE[] = {5, 3, 2, 1, 0};
int MAX_NUM_NODE = 162; // 81
int MAX_NUM_EDGE = 256; // 162

int AppMain(int argc, char *argv[])
{
    using namespace DrvAPI;

    if (myThreadId() == -1 && myCoreId() == -1) { return -1; }
    DrvAPIMemoryAllocatorInit();

    // should be (0, 1) or (1, 0), but now there is no config file
    // connects two PXNs
    CSRInterface csr(0, 0);

    uint64_t VArrSz = csr.VArrSz;
    uint64_t EArrSz = csr.EArrSz;

    DrvAPI::atomic_add<int>(&g_barrier, 1);
    // }

    // barrier to wait until loading finishes
    {
      int t = totalThreads(); 
      while (g_barrier != t) DrvAPI::wait(1000);
    }

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

    // statistics
    size_t sampledEdgeCnt = 0, sampledVertexCnt = 0;

    for (size_t i = beg; i < end; i++) {
      // printf("iteration %d\n", (int) i);
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
        Vertex V = csr.V(glbID);

        // Gather neighbors
        neighborhood_size = 0;

        uint64_t startEL = V.start;
        uint64_t endEL = startEL + V.edges;
        uint64_t num_neighbors = V.edges;

        uint64_t edges_to_fetch = std::min<uint64_t>(NUM_SAMPLE[level], num_neighbors);

        for (unsigned int i = 0; i < edges_to_fetch; ++i) {
          size_t v = rand() % num_neighbors;
          // ![REMOTE/LOCAL]
          Edge e = csr.E(startEL + v);
          neighborhood[neighborhood_size++] = e;
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

          if (!visited) {
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
          } else { // visited
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

      sampledEdgeCnt += edges_size;
      sampledVertexCnt += vertices_size;

      // ignore post processing steps
      // clear all data structures
      edges_size = 0;
      vertices_size = 0;
      frontier_head = frontier_tail = 0;
      neighborhood_size = 0;
    }

    printf("%2d done; work: %lu, sampled edges: %lu, sampled vertices: %lu\n", 
      tid, end - beg, sampledEdgeCnt, sampledVertexCnt);
    printf("avg sampled edges: %.2lf, avg sampled vertices: %.2lf\n", 
      ((double) sampledEdgeCnt) / (end - beg),
      ((double) sampledVertexCnt) / (end - beg)
      );

    printf("V local: %lu, V remote: %lu\n", csr.VLocalAccessCnt, csr.VRemoteAccessCnt);
    printf("E local: %lu, E remote: %lu\n", csr.ELocalAccessCnt, csr.ERemoteAccessCnt);

    return 0;
}
declare_drv_api_main(AppMain);
