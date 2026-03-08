# Install script for directory: /proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
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

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdrvapi.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdrvapi.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdrvapi.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE SHARED_LIBRARY FILES "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/build/api/libdrvapi.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdrvapi.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdrvapi.so")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdrvapi.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIAddress.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIAddressMap.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIAddressToNative.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIAllocator.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPICoreXY.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIGlobal.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPI.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIInfo.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIMain.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIMemory.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPINativeToAddress.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIOp.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIPointer.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIReadModifyWrite.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIThreadState.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPISysConfig.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPISystem.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIThread.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIVar.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPISection.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIBits.hpp"
    "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/api/DrvAPIDMA.hpp"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/proj/alanfaascache-PG0/late_feb/new/prev_commit/drv/build/api/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
