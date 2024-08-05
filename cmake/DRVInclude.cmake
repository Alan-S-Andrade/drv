include(ExternalProject)

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

# creates a drvx run target
# ${name} should be a target created with add_drvx_executable
# "ARGV" will be passed as the command line arguments to the drvx
# program at runtime
function (drvx_add_run_target run_target executable)
  if (NOT DEFINED ARCH_RV64)
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
      $<TARGET_PROPERTY:${run_target},DRV_MODEL_OPTIONS> # options for the model
      --num-pxn=$<TARGET_PROPERTY:${run_target},DRV_MODEL_NUM_PXN>
      --pxn-pods=$<TARGET_PROPERTY:${run_target},DRV_MODEL_PXN_PODS>
      --pod-cores=$<TARGET_PROPERTY:${run_target},DRV_MODEL_POD_CORES>
      --core-threads=$<TARGET_PROPERTY:${run_target},DRV_MODEL_CORE_THREADS>
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
      DRV_MODEL ${DRV_SOURCE_DIR}/tests/PANDOHammerDrvX.py
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
      Drv
      )
  endif()
endfunction()

# creates a drvr run target
# ${rvexecutable} should be a target created with add_drvr_executable
# ${cpexecutable} should be a target created with add_drvx_executable
# "ARGV" will be passed as the command line arguments to the drvr
# program at runtime
function (drvr_add_run_target_with_command_processor run_target rvexecutable cpexecutable)
  if (NOT DEFINED ARCH_RV64)
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
      --with-command-processor=$<TARGET_FILE:${cpexecutable}> # the command processor
      $<TARGET_PROPERTY:${run_target},DRV_MODEL_OPTIONS> # options for the model
      --num-pxn=$<TARGET_PROPERTY:${run_target},DRV_MODEL_NUM_PXN>
      --pxn-pods=$<TARGET_PROPERTY:${run_target},DRV_MODEL_PXN_PODS>
      --pod-cores=$<TARGET_PROPERTY:${run_target},DRV_MODEL_POD_CORES>
      --core-threads=$<TARGET_PROPERTY:${run_target},DRV_MODEL_CORE_THREADS>
      $<TARGET_FILE:RV64::${rvexecutable}> # the application to run
      $<TARGET_PROPERTY:${run_target},DRV_APPLICATION_ARGV> # arguments for the application
      2>&1 | tee $<TARGET_PROPERTY:${run_target},SST_RUN_DIR>/log.txt # log the output
      DEPENDS ${rvexecutable} Drv
      VERBATIM
      )
    set_target_properties(
      ${run_target}
      PROPERTIES
      SST_RUN_DIR ${CMAKE_CURRENT_BINARY_DIR}/${run_target}
      )
    set_target_properties(
      ${run_target}
      PROPERTIES
      DRV_MODEL ${DRV_SOURCE_DIR}/tests/PANDOHammerDrvR.py
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
      ${rvexecutable}
      ${cpexecutable}
      Drv
      )
  endif()
endfunction()

# set a property on a drvr target
function (drvr_set_run_target_properties target)
if (NOT DEFINED ARCH_RV64)
  set_target_properties(${target} ${ARGV})
endif()
endfunction()


# creates a drvr run target
# ${rvexecutable} should be a target created with add_drvr_executable
# "ARGV" will be passed as the command line arguments to the drvr
# program at runtime
function (drvr_add_run_target run_target rvexecutable)
  drvr_add_run_target_with_command_processor(${run_target} ${rvexecutable} pandocommand_loader)
endfunction()

# creates a drvr executable target
function (drvr_add_executable name)
  if (NOT DEFINED ARCH_RV64)
    ExternalProject_Add(
      ${name}
      SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
      BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/${name}-build
      INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}
      CONFIGURE_COMMAND
      cmake
      -DARCH_RV64=1
      -DCMAKE_C_COMPILER=${GNU_RISCV_TOOLCHAIN_PREFIX}/bin/riscv64-unknown-elfpandodrvsim-gcc
      -DCMAKE_SYSTEM_NAME=Generic
      -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}
      -DCMAKE_MODULE_PATH=${DRV_SOURCE_DIR}/cmake
      -DDRV_SOURCE_DIR=${DRV_SOURCE_DIR}
      -DDRV_BINARY_DIR=${DRV_BINARY_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}
      BUILD_COMMAND
      make ${name}
      INSTALL_COMMAND
      make install
      )
    add_executable(RV64::${name} IMPORTED DEPENDS ${name}_project)
    set_target_properties(RV64::${name} PROPERTIES IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/${name})
  else()
    message(STATUS "Adding drvr executable ${name}")
    set(SOURCES ${ARGV})
    list(POP_FRONT SOURCES)
    add_executable(${name} ${SOURCES})
    target_link_libraries(${name} pandohammer)
    install(TARGETS ${name} DESTINATION .)
  endif()
endfunction()

