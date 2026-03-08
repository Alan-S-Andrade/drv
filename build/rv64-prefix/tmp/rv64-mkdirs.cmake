# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv")
  file(MAKE_DIRECTORY "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv")
endif()
file(MAKE_DIRECTORY
  "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/build/rv64"
  "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/build/rv64"
  "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/build/rv64-prefix/tmp"
  "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/build/rv64-prefix/src/rv64-stamp"
  "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/build/rv64-prefix/src"
  "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/build/rv64-prefix/src/rv64-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/build/rv64-prefix/src/rv64-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/build/rv64-prefix/src/rv64-stamp${cfgdir}") # cfgdir has leading slash
endif()
