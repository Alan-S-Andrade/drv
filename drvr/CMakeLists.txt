set(DRVR_TESTS)
function (drvr_test executable)
  set(sources ${ARGN})
  drvr_add_executable(drvr_${executable} ${sources})
  drvr_add_run_target(drvr-run-${executable} drvr_${executable})
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
  drvr_target_link_options(drvr_${executable} PRIVATE -nostdlib)
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

# cycle
drvr_test_with_pandohammer(cycle cycle.c)
drvr_test_compile_options(cycle -DREADS=10)

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
  DRV_BUILD_CORE_THREADS 2
  DRV_BUILD_POD_CORES 2
  )


# lsu test
drvr_test_nostdlib(lsu lsu.S)

# malloc
drvr_test_with_pandohammer(malloc malloc.c malloc-main.c)

# multihart
drvr_test_with_pandohammer(multihart multihart.c)
drvr_test_build_properties(multihart
  PROPERTIES
  DRV_BUILD_CORE_THREADS 2
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
  DRV_BUILD_CORE_THREADS 2
  DRV_BUILD_POD_CORES 2
  )

# print_int
drvr_test_with_pandohammer(print_int print_int.c)

# puts
drvr_test_with_pandohammer(puts puts.c)

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

# spmm; broken; not sure why
if (FALSE)
  drvr_test_with_pandohammer(spmm spmm.cpp)
  drvr_test_inputs(spmm
    spmm.features
    spmm.graph.mtx
    )
endif()

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
