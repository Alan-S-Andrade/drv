# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/work2/10238/vineeth_architect/stampede3/drv_copy/drv")
  file(MAKE_DIRECTORY "/work2/10238/vineeth_architect/stampede3/drv_copy/drv")
endif()
file(MAKE_DIRECTORY
  "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/build_stampede/rv64"
  "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/build_stampede/rv64"
  "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/build_stampede/rv64-prefix/tmp"
  "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/build_stampede/rv64-prefix/src/rv64-stamp"
  "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/build_stampede/rv64-prefix/src"
  "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/build_stampede/rv64-prefix/src/rv64-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/build_stampede/rv64-prefix/src/rv64-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/work2/10238/vineeth_architect/stampede3/drv_copy/drv/build_stampede/rv64-prefix/src/rv64-stamp${cfgdir}") # cfgdir has leading slash
endif()
