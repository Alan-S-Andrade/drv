project(drv)
cmake_minimum_required(VERSION 3.25.0)

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

if (DEFINED SST_ENABLE_RISCV)
  if (NOT DEFINED GNU_RISCV_TOOLCHAIN_PREFIX)
    message(FATAL_ERROR "GNU_RISCV_TOOLCHAIN_PREFIX is not defined")
  endif()
endif()


set(CXX_STD "-std=c++17")

find_package(Boost REQUIRED COMPONENTS coroutine)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

add_subdirectory(interpreter)
add_subdirectory(api)
add_subdirectory(element)
add_subdirectory(cmake-drvx-example)

