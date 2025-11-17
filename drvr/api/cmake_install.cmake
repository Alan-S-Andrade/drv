# Install script for directory: /users/alanandr/lib/mydrv/api

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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE SHARED_LIBRARY FILES "/users/alanandr/lib/mydrv/drvr/api/libdrvapi.so")
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
    "/users/alanandr/lib/mydrv/api/DrvAPIAddress.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIAddressMap.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIAddressToNative.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIAllocator.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPICoreXY.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIGlobal.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPI.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIInfo.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIMain.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIMemory.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPINativeToAddress.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIOp.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIPointer.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIReadModifyWrite.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIThreadState.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPISysConfig.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPISystem.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIThread.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIVar.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPISection.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIBits.hpp"
    "/users/alanandr/lib/mydrv/api/DrvAPIDMA.hpp"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/users/alanandr/lib/mydrv/drvr/api/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
