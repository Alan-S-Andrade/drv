include(ExternalProject)
set(PROJECT_RV64_BINARY_DIR ${PROJECT_BINARY_DIR}/rv64)
if (NOT DEFINED RV64_PROJECT_NAME)
  set(RV64_PROJECT_NAME "rv64")
endif()

define_property(TARGET PROPERTY DRV_MODEL
  BRIEF_DOCS "The model to simulate"
  FULL_DOCS "The model to simulate"
  )

define_property(TARGET PROPERTY DRV_MODEL_OPTIONS
  BRIEF_DOCS "Parameters to pass to the model"
  FULL_DOCS "Parameters to pass to the model"
  )

define_property(TARGET PROPERTY DRV_MODEL_NUM_PXN
  BRIEF_DOCS "Number of PXNs"
  FULL_DOCS "Number of PXNs"
  )

define_property(TARGET PROPERTY DRV_MODEL_PXN_PODS
  BRIEF_DOCS "Number of pods per PXN"
  FULL_DOCS "Number of pods per PXN"
  )

define_property(TARGET PROPERTY DRV_MODEL_POD_CORES
  BRIEF_DOCS "Number of cores per pod"
  FULL_DOCS "Number of cores per pod"
  )

define_property(TARGET PROPERTY DRV_MODEL_CORE_THREADS
  BRIEF_DOCS "Number of threads per core"
  FULL_DOCS "Number of threads per core"
  )

define_property(TARGET PROPERTY DRV_BUILD_NUM_PXN
  BRIEF_DOCS "Number of PXNs (used for building)"
  FULL_DOCS "Number of PXNs (used for building)"
  )

define_property(TARGET PROPERTY DRV_BUILD_PXN_PODS
  BRIEF_DOCS "Number of pods per PXN (used for building)"
  FULL_DOCS "Number of pods per PXN (used for building)"
  )

define_property(TARGET PROPERTY DRV_BUILD_POD_CORES
  BRIEF_DOCS "Number of cores per pod (used for building)"
  FULL_DOCS "Number of cores per pod (used for building)"
  )

define_property(TARGET PROPERTY DRV_BUILD_CORE_THREADS
  BRIEF_DOCS "Number of threads per core (used for building)"
  FULL_DOCS "Number of threads per core (used for building)"
  )

define_property(TARGET PROPERTY DRV_APPLICATION_ARGV
  BRIEF_DOCS "Arguments to pass to the application"
  FULL_DOCS "Arguments to pass to the application"
  )

define_property(TARGET PROPERTY SST_SIM_OPTIONS
  BRIEF_DOCS "Parameters to pass to the simulator"
  FULL_DOCS "Parameters to pass to the simulator"
  )

define_property(TARGET PROPERTY SST_RUN_DIR
  BRIEF_DOCS "Directory in which to run the simulation"
  FULL_DOCS "Directory in which to run the simulation"
  )

define_property(TARGET PROPERTY DRV_MODEL_WITH_COMMANDPROCESSOR
  BRIEF_DOCS "The command processor will be used with the model"
  FULL_DOCS "The command processor will be used with the model"
  )

define_property(TARGET PROPERTY DRV_MODEL_COMMANDPROCESSOR
  BRIEF_DOCS "The command processor to use with the model"
  FULL_DOCS "The command processor to use with the model"
  )

# creates a drvx "executable" (really a shared library)
# will create a target "name"
function (drvx_add_executable name)
  if (NOT DEFINED ARCH_RV64)
    set(SOURCES ${ARGV})
    list(POP_FRONT SOURCES)
    add_library(${name} SHARED ${SOURCES})
    target_link_libraries(${name} PRIVATE drvapi)
  endif()
endfunction()

# set link libraries for a drvx target
function (drvx_target_link_libraries)
  if (NOT DEFINED ARCH_RV64)
    target_link_libraries(${ARGV})
  endif()
endfunction()

# creates a drv run target
# ${run_target} should be the name of the target to create
# ${executable} should be a target created with drv(x|r)_add_executable
# ${cpexecutable} should be a target created with drvx_add_executable
#
# it is left to the caller to set the properties of the run target
# particularly DRV_MODEL, DRV_MODEL_OPTIONS, DRV_APPLICATION_ARGV
function (drv_add_run_target run_target executable cpexecutable)
  if (NOT DEFINED ARCH_RV64)
    set(NO_CP "$<STREQUAL:${cpexecutable},>")
    set(CP "$<TARGET_FILE:${cpexecutable}>")
    set(CP_OPT "$<IF:${NO_CP},,--with-command-processor=${CP}>")

    # prefer the build properties if they are set, but fall back to the model properties
    set(BUILD_NUM_PXN_SET "$<BOOL:$<TARGET_PROPERTY:${run_target},DRV_BUILD_NUM_PXN>>")
    set(BUILD_NUM_PXN "$<TARGET_PROPERTY:${run_target},DRV_BUILD_NUM_PXN>")
    set(MODEL_NUM_PXN "$<IF:${BUILD_NUM_PXN_SET},${BUILD_NUM_PXN},$<TARGET_PROPERTY:${run_target},DRV_MODEL_NUM_PXN>>")

    set(BUILD_PXN_PODS_SET "$<BOOL:$<TARGET_PROPERTY:${run_target},DRV_BUILD_PXN_PODS>>")
    set(BUILD_PXN_PODS "$<TARGET_PROPERTY:${run_target},DRV_BUILD_PXN_PODS>")
    set(MODEL_PXN_PODS "$<IF:${BUILD_PXN_PODS_SET},${BUILD_PXN_PODS},$<TARGET_PROPERTY:${run_target},DRV_MODEL_PXN_PODS>>")

    set(BUILD_POD_CORES "$<TARGET_PROPERTY:${executable},DRV_BUILD_POD_CORES>")
    set(BUILD_POD_CORES_SET "$<BOOL:${BUILD_POD_CORES}>")
    set(MODEL_POD_CORES "$<IF:${BUILD_POD_CORES_SET},${BUILD_POD_CORES},$<TARGET_PROPERTY:${run_target},DRV_MODEL_POD_CORES>>")
    #set(MODEL_POD_CORES "$<IF:${BUILD_POD_CORES_SET},${BUILD_POD_CORES},${BUILD_POD_CORES}>")
    #set(MODEL_POD_CORES "$<IF:${BUILD_POD_CORES_SET},yes,no>")

    set(BUILD_CORE_THREADS "$<TARGET_PROPERTY:${executable},DRV_BUILD_CORE_THREADS>")
    set(BUILD_CORE_THREADS_SET "$<BOOL:${BUILD_CORE_THREADS}>")
    set(MODEL_CORE_THREADS "$<IF:${BUILD_CORE_THREADS_SET},${BUILD_CORE_THREADS},$<TARGET_PROPERTY:${run_target},DRV_MODEL_CORE_THREADS>>")

    add_custom_target(
      ${run_target}
      COMMAND
      mkdir -p $<TARGET_PROPERTY:${run_target},SST_RUN_DIR> &&
      cd $<TARGET_PROPERTY:${run_target},SST_RUN_DIR> &&
      PYTHONPATH=${DRV_SOURCE_DIR}/py:${DRV_SOURCE_DIR}/tests
      $<TARGET_FILE:SST::SST> # the simulator
      $<TARGET_PROPERTY:${run_target},SST_SIM_OPTIONS> # options for the simulator
      $<TARGET_PROPERTY:${run_target},DRV_MODEL> # the model to simulate
      --
      ${CP_OPT} # the command processor
      $<TARGET_PROPERTY:${run_target},DRV_MODEL_OPTIONS> # options for the model
      --num-pxn=${MODEL_NUM_PXN}
      --pxn-pods=${MODEL_PXN_PODS}
      --pod-cores=${MODEL_POD_CORES}
      --core-threads=${MODEL_CORE_THREADS}
      $<TARGET_FILE:${executable}> # the application to run
      $<TARGET_PROPERTY:${run_target},DRV_APPLICATION_ARGV> # arguments for the application
      2>&1 | tee $<TARGET_PROPERTY:${run_target},SST_RUN_DIR>/log.txt # log the output
      DEPENDS ${executable} Drv
      )
    set_target_properties(
      ${run_target}
      PROPERTIES
      SST_RUN_DIR ${CMAKE_CURRENT_BINARY_DIR}/${run_target}
      )
    set_target_properties(
      ${run_target}
      PROPERTIES
      DRV_MODEL_NUM_PXN 1
      DRV_MODEL_PXN_PODS 1
      DRV_MODEL_POD_CORES 1
      DRV_MODEL_CORE_THREADS 1
      )
    add_dependencies(
      ${run_target}
      ${executable}
      ${cpexecutable}
      Drv
      )
  endif()
endfunction()

# creates a drvx run target
# ${name} should be a target created with add_drvx_executable
# "ARGV" will be passed as the command line arguments to the drvx
# program at runtime
function (drvx_add_run_target_with_command_processor run_target executable cpexecutable)
  if (NOT DEFINED ARCH_RV64)
    drv_add_run_target(${run_target} ${executable} "${cpexecutable}")
    set_target_properties(
      ${run_target}
      PROPERTIES
      DRV_MODEL ${DRV_SOURCE_DIR}/tests/PANDOHammerDrvX.py 
      )
  endif()
endfunction()

# creates a drvx run target
# ${name} should be a target created with add_drvx_executable
# "ARGV" will be passed as the command line arguments to the drvx
# program at runtime
function (drvx_add_run_target run_target executable)
  drvx_add_run_target_with_command_processor(${run_target} ${executable} "")
endfunction()

# creates a drvr run target
# ${rvexecutable} should be a target created with add_drvr_executable
# ${cpexecutable} should be a target created with add_drvx_executable
# "ARGV" will be passed as the command line arguments to the drvr
# program at runtime
function (drvr_add_run_target_with_command_processor run_target rvexecutable cpexecutable)
  if (NOT DEFINED ARCH_RV64)
    drv_add_run_target(${run_target} ${rvexecutable} ${cpexecutable})
    set_target_properties(
      ${run_target}
      PROPERTIES
      DRV_MODEL ${DRV_SOURCE_DIR}/tests/PANDOHammerDrvR.py
      )
    add_dependencies(${run_target} ${rvexecutable})
  endif()
endfunction()

# creates a drvr run target
# ${rvexecutable} should be a target created with add_drvr_executable
# "ARGV" will be passed as the command line arguments to the drvr
# program at runtime
function (drvr_add_run_target run_target rvexecutable)
  drvr_add_run_target_with_command_processor(${run_target} ${rvexecutable} pandocommand_loader)
endfunction()

# set a property on a drv run target
function (drv_set_run_target_properties target)
if (NOT DEFINED ARCH_RV64)
  set_target_properties(${target} ${ARGV})
endif()
endfunction()

# set a property on a drvr target
function (drvr_set_run_target_properties target)
  drv_set_run_target_properties(${target} ${ARGV})
endfunction()

# set a property on a drvx target
function (drvx_set_run_target_properties target)
  drv_set_run_target_properties(${target} ${ARGV})
endfunction()

function (drvr_set_build_target_properties target)
  set_target_properties(${target} ${ARGV})
endfunction()

function (drvr_set_build_target_properties target)
  if (NOT DEFINED ARCH_RV64)
    set_target_properties(${target} ${ARGV})
  else()
    set_target_properties(${target} ${ARGV})
  endif()
endfunction()

function (drvr_target_compile_options target)
  if ( DEFINED ARCH_RV64 )
    target_compile_options(${target} ${ARGN})
  endif()
endfunction()

function (drvr_target_link_libraries target)
  if ( DEFINED ARCH_RV64 )
    target_link_libraries(${target} ${ARGN})
  else()
    add_dependencies(${target} ${ARGN})
  endif()
endfunction()

function (drvr_target_link_options target)
  if ( DEFINED ARCH_RV64 )
    target_link_options(${target} ${ARGN})
  endif()
endfunction()

# creates a drvr executable target
function (drvr_add_executable name)
  set(sources ${ARGN})
  if (NOT DEFINED ARCH_RV64)
    set(path ${CMAKE_CURRENT_BINARY_DIR})
    cmake_path(RELATIVE_PATH path BASE_DIRECTORY ${PROJECT_BINARY_DIR})
    add_executable(${name} IMPORTED)
    add_dependencies(${name} ${RV64_PROJECT_NAME})
    set_target_properties(
      ${name}
      PROPERTIES
      IMPORTED_LOCATION ${PROJECT_RV64_BINARY_DIR}/${path}/${name}
      DRV_BUILD_CORE_THREADS 1
      DRV_BUILD_POD_CORES 1
      DRV_BUILD_PXN_PODS 1
      DRV_BUILD_NUM_PXN 1
      )
  else()
    set(include_dir ${CMAKE_CURRENT_BINARY_DIR}/${name}_include)
    add_executable(${name} ${sources} ${include_dir}/address_map.h)
    add_custom_command(
      OUTPUT ${include_dir}/address_map.h
      COMMAND
      mkdir -p ${include_dir} &&
      python3 ${DRV_SOURCE_DIR}/py/addressmap.py
      --core-threads $<TARGET_PROPERTY:${name},DRV_BUILD_CORE_THREADS>
      --pod-cores $<TARGET_PROPERTY:${name},DRV_BUILD_POD_CORES>
      --pxn-pods $<TARGET_PROPERTY:${name},DRV_BUILD_PXN_PODS>
      --num-pxn $<TARGET_PROPERTY:${name},DRV_BUILD_NUM_PXN>
      cheader > ${include_dir}/address_map.h
      )
    target_include_directories(${name} PRIVATE ${include_dir})
    set_target_properties(${name}
      PROPERTIES
      DRV_BUILD_CORE_THREADS 1
      DRV_BUILD_POD_CORES 1
      DRV_BUILD_PXN_PODS 1
      DRV_BUILD_NUM_PXN 1
      )
  endif()
endfunction()

macro (drvr_rv64_build)
  if (NOT DEFINED ARCH_RV64)
    ExternalProject_Add(
      rv64
      SOURCE_DIR ${PROJECT_SOURCE_DIR}
      BINARY_DIR ${PROJECT_RV64_BINARY_DIR}
      INSTALL_DIR ${PROJECT_RV64_BINARY_DIR}
      CONFIGURE_COMMAND
      ${CMAKE_COMMAND}
      -DARCH_RV64=1
      -DCMAKE_MODULE_PATH=${CMAKE_MODULE_PATH}
      -DCMAKE_C_COMPILER=${GNU_RISCV_TOOLCHAIN_PREFIX}/bin/riscv64-unknown-elfpandodrvsim-gcc
      -DCMAKE_CXX_COMPILER=${GNU_RISCV_TOOLCHAIN_PREFIX}/bin/riscv64-unknown-elfpandodrvsim-g++
      -DCMAKE_SYSTEM_NAME=Generic
      -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}/rv64
      -DSST_CORE_PREFIX=${SST_CORE_PREFIX}
      -DSST_ELEMENTS_PREFIX=${SST_ELEMENTS_PREFIX}
      -DGNU_RISCV_TOOLCHAIN_PREFIX=${GNU_RISCV_TOOLCHAIN_PREFIX}
      ${PROJECT_SOURCE_DIR}
      INSTALL_COMMAND
      echo "Install command not needed"
      BUILD_ALWAYS 1
      )
  endif()
endmacro()

