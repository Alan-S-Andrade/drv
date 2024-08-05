define_property(TARGET PROPERTY DRV_MODEL_OPTIONS
  BRIEF_DOCS "Parameters to pass to the model"
  FULL_DOCS "Parameters to pass to the model"
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
function (add_drvx_executable name)
  set(SOURCES ${ARGV})
  list(POP_FRONT SOURCES)
  add_library(${name} SHARED ${SOURCES})
  target_link_libraries(${name} PRIVATE drvapi)
endfunction()

# creates a drvx run target
# ${name} should be a target created with add_drvx_executable
# "ARGV" will be passed as the command line arguments to the drvx
# program at runtime
function (add_drvx_run_target run_target executable)
  add_custom_target(
    ${run_target}
    COMMAND
    mkdir -p $<TARGET_PROPERTY:${run_target},SST_RUN_DIR> &&
    cd $<TARGET_PROPERTY:${run_target},SST_RUN_DIR> &&
    PYTHONPATH=${PROJECT_SOURCE_DIR}/py:${PROJECT_SOURCE_DIR}/tests
    $<TARGET_FILE:SST::SST> # the simulator
    $<TARGET_PROPERTY:${run_target},SST_SIM_OPTIONS> # options for the simulator
    ${PROJECT_SOURCE_DIR}/tests/PANDOHammerDrvX.py -- # the model to simulate
    $<TARGET_PROPERTY:${run_target},DRV_MODEL_OPTIONS> # options for the model
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
endfunction()

