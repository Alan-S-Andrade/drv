# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/users/alanandr/drv")
  file(MAKE_DIRECTORY "/users/alanandr/drv")
endif()
file(MAKE_DIRECTORY
  "/users/alanandr/drv/baseline_4c/rv64"
  "/users/alanandr/drv/baseline_4c/rv64"
  "/users/alanandr/drv/baseline_4c/rv64-prefix/tmp"
  "/users/alanandr/drv/baseline_4c/rv64-prefix/src/rv64-stamp"
  "/users/alanandr/drv/baseline_4c/rv64-prefix/src"
  "/users/alanandr/drv/baseline_4c/rv64-prefix/src/rv64-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/users/alanandr/drv/baseline_4c/rv64-prefix/src/rv64-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/users/alanandr/drv/baseline_4c/rv64-prefix/src/rv64-stamp${cfgdir}") # cfgdir has leading slash
endif()
