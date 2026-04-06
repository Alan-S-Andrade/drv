set(DRVR_TESTS)

function (drvr_test executable)
  set(sources ${ARGN})
  drvr_add_executable(drvr_${executable} ${sources})
  drvr_add_run_target(drvr-run-${executable} drvr_${executable})
  drvr_set_run_target_properties(drvr-run-${executable}
    PROPERTIES
    DRV_MODEL ${DRV_SOURCE_DIR}/model/drvr.py
    )
  drvr_set_build_target_properties(drvr_${executable}
    PROPERTIES
    DRV_BUILD_CORE_THREADS 1
    DRV_BUILD_POD_CORES_X 1
    DRV_BUILD_POD_CORES_Y 1
    )
endfunction()

function (drvr_test_with_pandohammer executable)
  set(sources ${ARGN})
  drvr_test(${executable} ${sources})
  drvr_target_link_libraries(drvr_${executable} pandohammer)
  set(DRVR_TESTS ${DRVR_TESTS} drvr-run-${executable} PARENT_SCOPE)
endfunction()

function (drvr_test_nostdlib executable)
  set(sources ${ARGN})
  drvr_test(${executable} ${sources})
  drvr_target_link_options(drvr_${executable}
    PRIVATE -nostdlib -T${PROJECT_SOURCE_DIR}/pandohammer/bsg_link.ld
    )
  set(DRVR_TESTS ${DRVR_TESTS} drvr-run-${executable} PARENT_SCOPE)
endfunction()

function (drvr_test_inputs executable)
  if (NOT DEFINED ARCH_RV64)
    set(inputs ${ARGN})
    get_target_property(RUN_DIR drvr-run-${executable} SST_RUN_DIR)
    foreach (input ${inputs})
      message(STATUS "Copying ${input} to ${RUN_DIR}/${input}")
      configure_file(${input} ${RUN_DIR}/${input} COPYONLY)
    endforeach()
  endif()
endfunction()

function (drvr_test_compile_options executable)
  set(flags ${ARGN})
  drvr_target_compile_options(drvr_${executable} PRIVATE ${flags})
endfunction()

function (drvr_test_build_properties executable)
  set(properties ${ARGN})
  drvr_set_build_target_properties(drvr_${executable} ${properties})
endfunction()

function (drvr_test_run_properties executable)
  set(properties ${ARGN})
  drvr_set_run_target_properties(drvr-run-${executable} ${properties})
endfunction()

# addressmap
drvr_test_with_pandohammer(addressmap addressmap.c)

# add
drvr_test_nostdlib(add add.S)

# amoswap
drvr_test_with_pandohammer(amoswap amoswap.c)

# test_posted_atomic (posted + blocking atomic correctness test)
drvr_test_with_pandohammer(test_posted_atomic test_posted_atomic.c)
drvr_test_build_properties(test_posted_atomic
  PROPERTIES
  DRV_BUILD_CORE_THREADS 4
)

# atomic_bw_blocking (scatter-add benchmark, blocking atomics, L2SP)
drvr_test_with_pandohammer(atomic_bw_blocking atomic_bw.cpp)
drvr_test_build_properties(atomic_bw_blocking
  PROPERTIES
  DRV_BUILD_POD_CORES_X 4
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
)
drvr_test_run_properties(atomic_bw_blocking
  PROPERTIES
  DRV_MODEL_OPTIONS --pod-l2sp-banks=1
)

# atomic_bw_posted (scatter-add benchmark, posted atomics, L2SP)
drvr_test_with_pandohammer(atomic_bw_posted atomic_bw.cpp)
drvr_test_compile_options(atomic_bw_posted -DUSE_POSTED)
drvr_test_build_properties(atomic_bw_posted
  PROPERTIES
  DRV_BUILD_POD_CORES_X 4
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
)
drvr_test_run_properties(atomic_bw_posted
  PROPERTIES
  DRV_MODEL_OPTIONS --pod-l2sp-banks=1
)

# cycle
drvr_test_with_pandohammer(cycle cycle.c)
drvr_test_compile_options(cycle -DREADS=5)
drvr_test_build_properties(cycle
  PROPERTIES
  DRV_BUILD_POD_CORES_X 1
  DRV_BUILD_POD_CORES_Y 2
  DRV_BUILD_CORE_THREADS 1
)

# exit
drvr_test_nostdlib(exit exit.S)

# fence
drvr_test_with_pandohammer(fence fence.cpp)

# fib
drvr_test_with_pandohammer(fib fib.c fib-main.c)

# float lsu
drvr_test_nostdlib(float_lsu float_lsu.S float_lsu.c)

# fma
drvr_test_nostdlib(fma fma.S fma.c)

# fma-multith
drvr_test_with_pandohammer(fma-multith fma-multith.cpp)
drvr_test_build_properties(fma-multith
  PROPERTIES
  DRV_BUILD_CORE_THREADS 8
  )

# fread
drvr_test_with_pandohammer(fread fread.c)
drvr_test_inputs(fread fread.txt)

# fscanf
drvr_test_with_pandohammer(fscanf fscanf.c)
drvr_test_inputs(fscanf fscanf.txt)

# fstat
drvr_test_with_pandohammer(fstat fstat.cpp)

# gups
math(EXPR gups_table_size "1024*1024")
drvr_test_with_pandohammer(gups gups.cpp)
drvr_test_compile_options(gups -DTHREAD_UPDATES=4 -DTABLE_SIZE=${gups_table_size})
drvr_test_build_properties(gups
  PROPERTIES
  DRV_BUILD_CORE_THREADS 1
  DRV_BUILD_POD_CORES 1
)
drvr_test_run_properties(gups
  PROPERTIES
  DRV_MODEL_OPTIONS --pxn-dram-banks=8
)

# lsu test
drvr_test_nostdlib(lsu lsu.S)

# malloc
drvr_test_with_pandohammer(malloc malloc.c malloc-main.c)

# leiden_single
drvr_test_with_pandohammer(leiden_single leiden_single.c)

# attention
drvr_test_with_pandohammer(attn_fixed attn_fixed.cpp)
drvr_test_build_properties(attn_fixed
  PROPERTIES
  DRV_BUILD_CORE_THREADS 1
  DRV_BUILD_POD_CORES_X 2
  DRV_BUILD_POD_CORES_Y 1
)
drvr_test_run_properties(attn_fixed
  PROPERTIES
  DRV_MODEL_OPTIONS --pxn-dram-banks=8
)


#dense_gemm
  drvr_test_with_pandohammer(dense_gemm dense_gemm.cpp)
  drvr_test_compile_options(dense_gemm -DDEBUG)
   drvr_test_build_properties(dense_gemm
  PROPERTIES
  DRV_BUILD_POD_CORES_X 4
  #DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
)


# bfs_sw_multi
  drvr_test_with_pandohammer(bfs_multi_sw bfs_sw_hw_multi_final.cpp)
  drvr_test_compile_options(bfs_multi_sw -DDEBUG)
  drvr_test_build_properties(bfs_multi_sw
  PROPERTIES
  DRV_BUILD_POD_CORES_X 4
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
)
drvr_test_run_properties(bfs_multi_sw
  PROPERTIES
  DRV_MODEL_OPTIONS --dram-access-time=70ns
)

# stream_bw_l2sp (streaming bandwidth benchmark, L2SP, no barriers)
drvr_test_with_pandohammer(stream_bw_l2sp stream_bw_l2sp.cpp)
drvr_test_build_properties(stream_bw_l2sp
  PROPERTIES
  DRV_BUILD_POD_CORES_X 8
  DRV_BUILD_POD_CORES_Y 2
  DRV_BUILD_CORE_THREADS 16
)
drvr_test_run_properties(stream_bw_l2sp
  PROPERTIES
  DRV_MODEL_OPTIONS --pod-l2sp-banks=1
  #DRV_MODEL_OPTION0 --pod-l2sp-interleave=64
)

# shared_read_l2sp (shared-read benchmark, L2SP, all harts read same array)
drvr_test_with_pandohammer(shared_read_l2sp shared_read_l2sp.cpp)
drvr_test_build_properties(shared_read_l2sp
  PROPERTIES
  DRV_BUILD_POD_CORES_X 2
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
)
drvr_test_run_properties(shared_read_l2sp
  PROPERTIES
  DRV_MODEL_OPTIONS --pod-l2sp-banks=1
  #DRV_MODEL_OPTION0 --pod-l2sp-interleave=64
)

# bfs_sw_multi_l2sp (L2SP-optimized version)
drvr_test_with_pandohammer(bfs_multi_sw_l2sp bfs_sw_hw_multi_l2sp.cpp)
drvr_test_compile_options(bfs_multi_sw_l2sp -DDEBUG)
drvr_test_build_properties(bfs_multi_sw_l2sp
  PROPERTIES
  DRV_BUILD_POD_CORES_X 8
  DRV_BUILD_POD_CORES_Y 8
  DRV_BUILD_CORE_THREADS 16
)
drvr_test_run_properties(bfs_multi_sw_l2sp
  PROPERTIES
  DRV_MODEL_OPTIONS --dram-access-time=70ns
  DRV_MODEL_OPTION0 --pod-l2sp-banks=4
  DRV_MODEL_OPTION1 --pod-l2sp-interleave=64
)

# bfs_csr_weak (CSR graph BFS with weak scaling, L2SP frontiers)
drvr_test_with_pandohammer(bfs_csr_weak bfs_csr_weak.cpp)
drvr_test_build_properties(bfs_csr_weak
  PROPERTIES
  DRV_BUILD_POD_CORES_X 1
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
)
drvr_test_run_properties(bfs_csr_weak
  PROPERTIES
  DRV_MODEL_OPTIONS --dram-access-time=40ns
  DRV_MODEL_OPTION0 --pod-l2sp-banks=1
  DRV_MODEL_OPTION1 --pod-l2sp-interleave=64
  DRV_MODEL_OPTION2 --pxn-dram-banks=5
  DRV_MODEL_OPTION3 --pxn-dram-interleave=64
)

# bfs_csr_weak_multi (Multi-pod CSR graph BFS with weak scaling)
drvr_test_with_pandohammer(bfs_csr_weak_multi bfs_csr_weak_multi.cpp)
drvr_test_build_properties(bfs_csr_weak_multi
  PROPERTIES
  DRV_BUILD_POD_CORES_X 1
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
  DRV_BUILD_PXN_PODS 2
)
drvr_test_run_properties(bfs_csr_weak_multi
  PROPERTIES
  DRV_MODEL_OPTIONS --dram-access-time=40ns
  DRV_MODEL_OPTION0 --pod-l2sp-banks=1
  DRV_MODEL_OPTION1 --pod-l2sp-interleave=64
  DRV_MODEL_OPTION2 --pxn-dram-banks=5
  DRV_MODEL_OPTION3 --pxn-dram-interleave=64
)

# bfs_sw_multi_barrier (improved barrier, NO L2SP for data - for comparison)
drvr_test_with_pandohammer(bfs_multi_sw_barrier bfs_sw_hw_multi_barrier.cpp)
drvr_test_compile_options(bfs_multi_sw_barrier -DDEBUG)
drvr_test_build_properties(bfs_multi_sw_barrier
  PROPERTIES
  DRV_BUILD_POD_CORES_X 4
  DRV_BUILD_POD_CORES_Y 8
  DRV_BUILD_CORE_THREADS 16
)
drvr_test_run_properties(bfs_multi_sw_barrier
  PROPERTIES
  DRV_MODEL_OPTIONS --dram-access-time=70ns
)

#bfs_multihart
drvr_test_with_pandohammer(bfs_multihart bfs_multihart.cpp)
  drvr_test_build_properties(bfs_multihart
  PROPERTIES
  DRV_BUILD_POD_CORES_X 2
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
  )

# naive work stealing
drvr_test_with_pandohammer(work_stealing work_stealing_benchmark.cpp)
drvr_test_build_properties(work_stealing
  PROPERTIES
  DRV_BUILD_POD_CORES_X 8
  DRV_BUILD_POD_CORES_Y 4
  DRV_BUILD_CORE_THREADS 16
  )

# work queue depth trace
drvr_test_with_pandohammer(work_queue_depth_trace work_queue_depth_trace.cpp)
drvr_test_build_properties(work_queue_depth_trace
  PROPERTIES
  DRV_BUILD_POD_CORES_X 2
  DRV_BUILD_POD_CORES_Y 4
  DRV_BUILD_CORE_THREADS 16
  )

# work stealing batch
drvr_test_with_pandohammer(work_stealing_batch work_stealing_benchmark_batch.cpp)
drvr_test_build_properties(work_stealing_batch
  PROPERTIES
  DRV_BUILD_POD_CORES_X 2
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
  )


# BFS with work stealing
drvr_test_with_pandohammer(bfs_work_stealing bfs_work_stealing.cpp)
drvr_test_build_properties(bfs_work_stealing
  PROPERTIES
  DRV_BUILD_POD_CORES_X 2
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
  )

drvr_test_with_pandohammer(bfs_nosteal_baseline bfs_work_stealing_nosteal_baseline.cpp)
drvr_test_build_properties(bfs_nosteal_baseline
  PROPERTIES
  DRV_BUILD_POD_CORES_X 8
  DRV_BUILD_POD_CORES_Y 8
  DRV_BUILD_CORE_THREADS 16
  )

drvr_test_with_pandohammer(bfs_work_stealing_adaptive bfs_work_stealing_adaptive.cpp)
drvr_test_build_properties(bfs_work_stealing_adaptive
  PROPERTIES
  DRV_BUILD_POD_CORES_X 2
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
  )

drvr_test_with_pandohammer(bfs_work_stealing_l1sp_cache bfs_work_stealing_l1sp_cache.cpp)
drvr_test_build_properties(bfs_work_stealing_l1sp_cache
  PROPERTIES
  DRV_BUILD_POD_CORES_X 2
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
  )

drvr_test_with_pandohammer(bfs_l1sp_cache_baseline bfs_work_stealing_l1sp_cache.cpp)
drvr_test_compile_options(bfs_l1sp_cache_baseline -DNO_STEAL_BASELINE)
drvr_test_build_properties(bfs_l1sp_cache_baseline
  PROPERTIES
  DRV_BUILD_POD_CORES_X 2
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
  )

  # BFS imbalanced baseline
 drvr_test_with_pandohammer(bfs_imbalanced_baseline bfs_imbalanced_baseline.cpp)
 drvr_test_build_properties(bfs_imbalanced_baseline
   PROPERTIES
   DRV_BUILD_POD_CORES_X 8
   DRV_BUILD_POD_CORES_Y 8
   DRV_BUILD_CORE_THREADS 16
   )

drvr_test_with_pandohammer(static_work static_work_benchmark.cpp)
drvr_test_build_properties(static_work
  PROPERTIES
  DRV_BUILD_POD_CORES_X 8
  DRV_BUILD_POD_CORES_Y 4
  DRV_BUILD_CORE_THREADS 16
  )

# PageRank baseline (no work stealing)
drvr_test_with_pandohammer(pagerank_baseline pagerank_baseline.cpp)
drvr_test_build_properties(pagerank_baseline
  PROPERTIES
  DRV_BUILD_POD_CORES_X 8
  DRV_BUILD_POD_CORES_Y 8
  DRV_BUILD_CORE_THREADS 16
  )

# PageRank with work stealing (TTAS)
drvr_test_with_pandohammer(pagerank_stealing pagerank_stealing.cpp)
drvr_test_build_properties(pagerank_stealing
  PROPERTIES
  DRV_BUILD_POD_CORES_X 8
  DRV_BUILD_POD_CORES_Y 8
  DRV_BUILD_CORE_THREADS 16
  )

drvr_test_with_pandohammer(l2sp_latency_minimal l2sp_latency_minimal.cpp)
drvr_test_build_properties(l2sp_latency_minimal
  PROPERTIES
  DRV_BUILD_POD_CORES_X 8
  DRV_BUILD_POD_CORES_Y 8
  DRV_BUILD_CORE_THREADS 16
)

# bfs
drvr_test_with_pandohammer(bfs bfs_grid.cpp)
drvr_test_build_properties(bfs
  PROPERTIES
  DRV_BUILD_POD_CORES_X 1
  # DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 0
)

# bfs multi
drvr_test_with_pandohammer(bfs-multith bfs_grid_multith.cpp)
drvr_test_compile_options(bfs-multith -DDEBUG)
drvr_test_build_properties(bfs-multith
  PROPERTIES
  DRV_BUILD_POD_CORES_X 4
  # DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 4
)

#tc
drvr_test_with_pandohammer(tc tc.c)
drvr_test_build_properties(tc
  PROPERTIES
  DRV_BUILD_POD_CORES_X 1
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 8
)

#tc_larger
drvr_test_with_pandohammer(tc_larger tc_larger.cpp)

# tc_galois
#drvr_test_with_pandohammer(tc_galois tc_galois.cpp)

# linked_list
drvr_test_with_pandohammer(list_traverse list_traverse.cpp)
drvr_test_compile_options(list_traverse -O2 -g -DNDEBUG)
drvr_test_build_properties(list_traverse
  PROPERTIES
  DRV_BUILD_POD_CORES_X 2
  DRV_BUILD_POD_CORES_Y 2
  DRV_BUILD_CORE_THREADS 1
)

#ptr_chase
drvr_test_with_pandohammer(ptr_chase ptr_chase.cpp)
drvr_test_build_properties(ptr_chase
  PROPERTIES
  DRV_BUILD_POD_CORES_X 1
  # DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 3
)

# gemm_int_rand
drvr_test_with_pandohammer(gemm_int_rand gemm_int.cpp)

# gemm_int_deter
drvr_test_with_pandohammer(gemm_int_deter gemm_int_deter.cpp)
drvr_test_inputs(gemm_int_deter A.bin)
drvr_test_inputs(gemm_int_deter B.bin)

# multihart
drvr_test_with_pandohammer(multihart multihart.c)
drvr_test_build_properties(multihart
  PROPERTIES
  DRV_BUILD_CORE_THREADS 4
)

# ph_hello
drvr_test_with_pandohammer(ph_hello ph_hello.c)

# poke
drvr_test_with_pandohammer(poke poke.c)
drvr_test_compile_options(poke -DSTORE_ADDR=0 -DSTORE_VALUE=0)

# printf
drvr_test_with_pandohammer(printf printf.c)
drvr_test_build_properties(printf
  PROPERTIES
  DRV_BUILD_CORE_THREADS 1
)

# print_int
drvr_test_with_pandohammer(print_int print_int.c)

# puts
drvr_test_with_pandohammer(puts puts.c)
drvr_set_build_target_properties(drvr_puts
  PROPERTIES
  DRV_BUILD_POD_CORES 2
  DRV_BUILD_CORE_THREADS 1
  )
# read
drvr_test_with_pandohammer(read read.c)
drvr_test_inputs(read read.txt)

# shift
drvr_test_nostdlib(shift shift.S)

# simple
drvr_test_with_pandohammer(simple simple.c simple-main.c)

# snprintf
drvr_test_with_pandohammer(snprintf snprintf-main.c snprintf-test.cpp)

# vread
drvr_test_with_pandohammer(vread vread.cpp vread.hpp vsize.cpp)
drvr_test_compile_options(vread -DVSIZE=64)

# write
drvr_test_with_pandohammer(write write.c)

drvr_test_with_pandohammer(wait-with-sleep wait.c)
drvr_test_compile_options(wait-with-sleep -DUSE_SLEEP=1)
drvr_set_build_target_properties(drvr_wait-with-sleep
  PROPERTIES
  DRV_BUILD_POD_CORES 1
  DRV_BUILD_CORE_THREADS 2
  )

drvr_test_compile_options(wait-with-sleep -DUSE_SLEEP=1)
drvr_set_build_target_properties(drvr_wait-with-sleep
  PROPERTIES
  DRV_BUILD_POD_CORES 8
  DRV_BUILD_CORE_THREADS 8
  )

drvr_test_with_pandohammer(wait-without-sleep wait.c)

# spmm; broken; not sure why
if (FALSE)
  drvr_test_with_pandohammer(spmm spmm.cpp)
  drvr_test_inputs(spmm
    spmm.features
    spmm.graph.mtx
    )
endif()

# PageRank on RMAT graph (imbalanced baseline, for roofline comparison with BFS)
drvr_test_with_pandohammer(pagerank_rmat pagerank_rmat.cpp)
drvr_test_build_properties(pagerank_rmat
  PROPERTIES
  DRV_BUILD_POD_CORES_X 2
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
)

# BFS with perfect work stealing — L1SP/L2SP utilization showcase
drvr_test_with_pandohammer(bfs_ws_utilization bfs_ws_utilization.cpp)
drvr_test_build_properties(bfs_ws_utilization
  PROPERTIES
  DRV_BUILD_POD_CORES_X 2
  DRV_BUILD_POD_CORES_Y 1
  DRV_BUILD_CORE_THREADS 16
)

# open; currently broken (fix would be in RISCVSimulator)
if (FALSE)
  drvr_test_with_pandohammer(open open.c)
  drvr_test_run_properties(open
    PROPERTIES
    DRV_MODEL_OPTIONS --verbose=100
    )
endif()

# run all tests
if (NOT DEFINED ARCH_RV64)
  add_custom_target(
    drvr-run-all
    DEPENDS "${DRVR_TESTS}"
    )
endif()
