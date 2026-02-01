# Install script for directory: /work2/10238/vineeth_architect/stampede3/drv_copy/drv/pandohammer

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/work/.local/rv64")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/install/bin/riscv64-unknown-elfpandodrvsim-objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE FILE FILES
    "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/pandohammer/crt.S"
    "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/pandohammer/lock.c"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/pandohammer" TYPE FILE FILES
    "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/pandohammer/pandohammer/address.h"
    "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/pandohammer/pandohammer/atomic.h"
    "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/pandohammer/pandohammer/cpuinfo.h"
    "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/pandohammer/pandohammer/mmio.h"
    "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/pandohammer/pandohammer/staticdecl.h"
    "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/pandohammer/pandohammer/stringify.h"
    "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/pandohammer/pandohammer/hartsleep.h"
    "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/pandohammer/pandohammer/register.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE FILE FILES "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/pandohammer/bsg_link.ld")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/build_stampede/rv64/pandohammer/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
