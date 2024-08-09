cmake_minimum_required(VERSION 3.30.0)
project(drv LANGUAGES CXX C ASM)

include(ExternalProject)

if (NOT DEFINED SST_CORE_PREFIX)
  message(FATAL_ERROR "SST_CORE_PREFIX is not defined")
endif()
set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${SST_CORE_PREFIX})
find_package(SST)

if (NOT DEFINED SST_ELEMENTS_PREFIX)
  message(FATAL_ERROR "SST_ELEMENTS_PREFIX is not defined")
endif()

if (NOT DEFINED GNU_RISCV_TOOLCHAIN_PREFIX)
  message(FATAL_ERROR "GNU_RISCV_TOOLCHAIN_PREFIX is not defined")
endif()


set(DRV_SOURCE_DIR ${PROJECT_SOURCE_DIR})
set(DRV_BINARY_DIR ${PROJECT_BINARY_DIR})

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
find_package(DRV REQUIRED)

set(CXX_STD "-std=c++17")

if (NOT DEFINED ARCH_RV64)
  ExternalProject_Add(
    rv64
    SOURCE_DIR ${DRV_SOURCE_DIR}
    BINARY_DIR ${PROJECT_RV64_BINARY_DIR}
    INSTALL_DIR ${PROJECT_RV64_BINARY_DIR}
    CONFIGURE_COMMAND
    cmake
    -DARCH_RV64=1
    -DCMAKE_C_COMPILER=${GNU_RISCV_TOOLCHAIN_PREFIX}/bin/riscv64-unknown-elfpandodrvsim-gcc
    -DCMAKE_CXX_COMPILER=${GNU_RISCV_TOOLCHAIN_PREFIX}/bin/riscv64-unknown-elfpandodrvsim-g++
    -DCMAKE_SYSTEM_NAME=Generic
    -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}/rv64
    -DSST_CORE_PREFIX=${SST_CORE_PREFIX}
    -DSST_ELEMENTS_PREFIX=${SST_ELEMENTS_PREFIX}
    -DGNU_RISCV_TOOLCHAIN_PREFIX=${GNU_RISCV_TOOLCHAIN_PREFIX}
    ${CMAKE_CURRENT_SOURCE_DIR}
    BUILD_ALWAYS 1
    )
endif()


add_subdirectory(interpreter)
add_subdirectory(api)
add_subdirectory(element)
add_subdirectory(pandocommand)
add_subdirectory(pandohammer)
add_subdirectory(cmake-drvx-example)
add_subdirectory(cmake-drvr-example)
add_subdirectory(cmake)
add_subdirectory(drvx)
add_subdirectory(drvr)
