find_package(DRV)

# create a new executable target
add_drvx_executable(drvx-example main.cpp)

# create a run target for the executable
add_drvx_run_target(run-drvx-example-hello drvx-example)

# use two threads to simulate
set_target_properties(run-drvx-example-hello PROPERTIES
  SST_SIM_OPTIONS "-n 2")
# pass the argument "hello" to the application
set_target_properties(run-drvx-example-hello PROPERTIES
  DRV_APPLICATION_ARGV "hello")
# model one pxn, one pod, one core, one thread
set_target_properties(run-drvx-example-hello PROPERTIES
  DRV_MODEL_OPTIONS "--num-pxn=1 --pod-cores=1 --pxn-pods=1 --core-threads=1")

add_drvx_run_target(run-drvx-example-world drvx-example)
set_target_properties(run-drvx-example-world PROPERTIES
  DRV_APPLICATION_ARGV "world")
set_target_properties(run-drvx-example-world PROPERTIES
  DRV_MODEL_OPTIONS "--num-pxn=1 --pod-cores=1 --pxn-pods=1 --core-threads=1")
