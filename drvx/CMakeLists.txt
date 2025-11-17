if (NOT DEFINED ARCH_RV64)
  set(DRVX_TESTS)
  function (drvx_test name)
    drvx_add_executable(${name} ${name}.cpp)
    drvx_add_run_target(drvx-run-${name} ${name})
    drvx_set_run_target_properties(
      drvx-run-${name}
      PROPERTIES
      DRV_MODEL ${DRV_SOURCE_DIR}/model/hammerblade-x.py
      DRV_MODEL_NUM_PXN 1
      DRV_MODEL_PXN_PODS 1
      DRV_MODEL_POD_CORES_X 1
      DRV_MODEL_POD_CORES_Y 1      
      DRV_MODEL_CORE_THREADS 1
      )
    set(DRVX_TESTS ${DRVX_TESTS} drvx-run-${name} PARENT_SCOPE)
  endfunction()

  function (drvx_test_complex_with_cp name cp_name)
    drvx_add_executable(${name} ${ARGN})
    drvx_add_run_target_with_command_processor(drvx-run-${name} ${name} ${cp_name})
    drvx_set_run_target_properties(
      drvx-run-${name}
      PROPERTIES
      DRV_MODEL ${DRV_SOURCE_DIR}/model/mesh.py
      DRV_MODEL_NUM_PXN 1
      DRV_MODEL_PXN_PODS 1
      DRV_MODEL_PODS_CORES_X 1
      DRV_MODEL_PODS_CORES_Y 1
      DRV_MODEL_CORE_THREADS 1
      )
    set(DRVX_TESTS ${DRVX_TESTS} drvx-run-${name} PARENT_SCOPE)
  endfunction()

  function (drvx_test_complex name)
    drvx_test_complex_with_cp(${name} "" ${ARGN})
  endfunction()

  drvx_test(simple) # google test candidate
  drvx_test(leiden_single)
  drvx_test(tc)
  drvx_test(allocator) # google test candidate
  drvx_test(atomic_read_back) # google test candidate
  drvx_test(amoadd) # tough test with google test
  drvx_set_run_target_properties(
    drvx-run-amoadd
    PROPERTIES
    DRV_MODEL_CORE_THREADS 2
    )
  drvx_test(argv) # google test candidate
  drvx_set_run_target_properties(
    drvx-run-argv
    PROPERTIES
    DRV_APPLICATION_ARGV "this should have 6 arguments"
    )
  drvx_test(cas) # google test candidate
  drvx_test(comm) # google test candidate (needs a timeout maybe)
  drvx_set_run_target_properties(
    drvx-run-comm
    PROPERTIES
    DRV_MODEL_PODS_CORES 2
    )

  drvx_test(fence) # google test candidate
  drvx_set_run_target_properties(
    drvx-run-fence
    PROPERTIES
    DRV_MODEL_CORE_THREADS 2
    )

  drvx_test(globals) # google test candidate
  drvx_test(info) # google test candidate? maybe a tough one
  drvx_set_run_target_properties(
    drvx-run-info
    PROPERTIES
    DRV_MODEL_POD_CORES_X 3
    DRV_MODEL_POD_CORES_Y 2
    )
  drvx_test(lock) # google test candidate
  drvx_set_run_target_properties(
    drvx-run-lock
    PROPERTIES
    DRV_MODEL_POD_CORES 2
    )

  drvx_test(mem) # google test candidate
  drvx_test(multicore) # google test candidate? (needs a timeout maybe)
  drvx_set_run_target_properties(
    drvx-run-multicore
    PROPERTIES
    DRV_MODEL_POD_CORES 2
    )
  drvx_test(multicoreamoadd) # google test candidate? (needs a timeout maybe)
  drvx_set_run_target_properties(
    drvx-run-multicoreamoadd
    PROPERTIES
    DRV_MODEL_CORE_THREADS 2
    )
  drvx_test(addrmap) # tough test with google test
  drvx_test(nop) # google test candidate
  drvx_test(pointer) # google test candidate

  drvx_test_complex_with_cp(super_simple_task_runtime super_simple_task_runtime runtime.cpp runtime_main.cpp) # google test candidate
  drvx_set_run_target_properties(
    drvx-run-super_simple_task_runtime
    PROPERTIES
    DRV_MODEL_NUM_PXN 2
    DRV_MODEL_POD_CORES 2
    )

  drvx_test(to_address) # google test candidate
  drvx_test(to_native) # google test candidate
  drvx_set_run_target_properties(
    drvx-run-to_address
    PROPERTIES
    DRV_MODEL_OPTIONS "--drvx-stack-in-l1sp"
    )

  # drvx_test(commandprocessor)
  # drvx_set_run_target_properties(
  #   drvx-run-commandprocessor
  #   PROPERTIES
  #   DRV_MODEL_OPTIONS --with-commandprocesor=$<TARGET_FILE:commandprocessor>
  #   )

  drvx_test(gups) # more of a microbenchmark than a test
  drvx_test(gups_multi_node) # more of a microbenchmark than a test

  math(EXPR gups_table_size "1 << 26")
  math(EXPR gups_thread_updates "1<<4")
  drvx_set_run_target_properties(
    drvx-run-gups
    PROPERTIES
    DRV_MODEL_POD_CORES_X 16
    DRV_MODEL_POD_CORES_Y 16
    DRV_MODEL_PXN_PODS 1
    DRV_MODEL_OPTIONS "--pod-l2sp-banks=1 --pxn-dram-banks=4"
    DRV_APPLICATION_ARGV "${gups_table_size} ${gups_thread_updates}"
    )
  drvx_set_run_target_properties(
    drvx-run-gups_multi_node
    PROPERTIES
    DRV_APPLICATION_ARGV "${gups_table_size} ${gups_thread_updates}"
    DRV_MODEL_NUM_PXN 2
    )
  drvx_test(stream)

  drvx_test(stride)
  drvx_set_run_target_properties(
    drvx-run-stride
    PROPERTIES
    DRV_MODEL_POD_CORES 1
    DRV_MODEL_CORE_THREADS 1
    DRV_APPLICATION_ARGV "1000 64"
    )

  drvx_test(flushinv)
  drvx_set_run_target_properties(
    drvx-run-flushinv
    PROPERTIES
    DRV_MODEL_NUM_PXN 1
    DRV_MODEL_POD_CORES 1
    DRV_MODEL_CORE_THREADS 1
    )

  drvx_test(dma)
  drvx_set_run_target_properties(
    drvx-run-dma
    PROPERTIES
    DRV_MODEL_NUM_PXN 1
    DRV_MODEL_POD_CORES 1
    DRV_MODEL_CORE_THREADS 1
    )

  add_custom_target(drvx-run-all
    DEPENDS "${DRVX_TESTS}"
    )
endif()
