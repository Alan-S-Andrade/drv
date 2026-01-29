# Install script for directory: /work2/10238/vineeth_architect/stampede3/drv/api

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/work/.local")
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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE SHARED_LIBRARY FILES "/work2/10238/vineeth_architect/stampede3/drv/build_stampede/api/libdrvapi.so")
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
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIAddress.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIAddressMap.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIAddressToNative.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIAllocator.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPICoreXY.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIGlobal.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPI.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIInfo.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIMain.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIMemory.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPINativeToAddress.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIOp.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIPointer.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIReadModifyWrite.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIThreadState.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPISysConfig.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPISystem.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIThread.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIVar.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPISection.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIBits.hpp"
    "/work2/10238/vineeth_architect/stampede3/drv/api/DrvAPIDMA.hpp"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/work2/10238/vineeth_architect/stampede3/drv/build_stampede/api/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
