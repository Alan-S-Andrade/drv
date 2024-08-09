drvx_add_executable(
  command_processor
  command_processor.cpp
  )

drvx_target_link_libraries(
  command_processor
  PRIVATE
  pandocommand
  )

drvr_add_executable(
  drvr-example
  hello.c
  )

drvr_target_link_libraries(
  drvr-example
  PRIVATE
  pandohammer
  )

drvr_set_build_target_properties(
  drvr-example
  PROPERTIES
  DRV_BUILD_NUM_PXN      1
  DRV_BUILD_PXN_PODS     1
  DRV_BUILD_POD_CORES    1
  DRV_BUILD_CORE_THREADS 1
  )

drvr_add_run_target(
  run-drvr-example
  drvr-example
  )

drvr_add_run_target(
  run-drvr-example-with-command-processor
  drvr-example
  command_processor
  )
