cmake_minimum_required(VERSION 3.30.0)
project(drv)

include(ExternalProject)

if (NOT DEFINED SST_CORE_PREFIX)
  message(FATAL_ERROR "SST_CORE_PREFIX is not defined")
endif()
set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${SST_CORE_PREFIX})
find_package(SST)

if (NOT DEFINED SST_ELEMENTS_PREFIX)
  message(FATAL_ERROR "SST_ELEMENTS_PREFIX is not defined")
endif()
# define sst element libraries that we need to link against
add_library(memHierarchy SHARED IMPORTED)
set_target_properties(memHierarchy PROPERTIES IMPORTED_LOCATION ${SST_ELEMENTS_PREFIX}/lib/sst-elements-library/libmemHierarchy.so)

if (NOT DEFINED GNU_RISCV_TOOLCHAIN_PREFIX)
  message(FATAL_ERROR "GNU_RISCV_TOOLCHAIN_PREFIX is not defined")
endif()

set(CXX_STD "-std=c++17")

find_package(Boost REQUIRED COMPONENTS coroutine)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(DRV_SOURCE_DIR ${PROJECT_SOURCE_DIR})
set(DRV_BINARY_DIR ${PROJECT_BINARY_DIR})

find_package(DRV REQUIRED)

ExternalProject_Add(
  pandohammer_project
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/pandohammer
  BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/pandohammer
  INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/pandohammer
  CONFIGURE_COMMAND
  cmake
  -DCMAKE_C_COMPILER=${GNU_RISCV_TOOLCHAIN_PREFIX}/bin/riscv64-unknown-elfpandodrvsim-gcc
  -DCMAKE_SYSTEM_NAME=Generic
  -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}/pandohammer
  ${CMAKE_CURRENT_SOURCE_DIR}/pandohammer
  # BUILD_COMMAND make
  # INSTALL_COMMAND make install
  )

add_library(pandohammer STATIC IMPORTED)
file(GLOB PANDOHAMMER_HEADERS ${CMAKE_CURRENT_BINARY_DIR}/pandohammer/include/pandohammer/*.h)

set_target_properties(pandohammer PROPERTIES
  IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/pandohammer/lib/libpandohammer.a
  INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_BINARY_DIR}/pandohammer/include
  PUBLIC_HEADER "${PANDOHAMMER_HEADERS}"
  )

add_subdirectory(interpreter)
add_subdirectory(api)
add_subdirectory(element)
add_subdirectory(pandocommand)
add_subdirectory(cmake-drvx-example)
add_subdirectory(cmake-drvr-example)
add_subdirectory(cmake)
add_subdirectory(drvx)
