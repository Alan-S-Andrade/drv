find_package(DRV REQUIRED)
add_library(
  pandocommand
  SHARED
  control.cpp
  loader.cpp
  )

target_include_directories(
  pandocommand
  PUBLIC
  ${PROJECT_SOURCE_DIR}
  )

target_link_libraries(
  pandocommand
  PUBLIC
  drvapi
  )

drvx_add_executable(
  pandocommand_loader
  default.cpp
  )

target_link_libraries(
  pandocommand_loader
  PUBLIC
  pandocommand
  )
