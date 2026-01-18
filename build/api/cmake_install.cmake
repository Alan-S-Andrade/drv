# Install script for directory: /users/alanandr/2/drv/api

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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE SHARED_LIBRARY FILES "/users/alanandr/2/drv/build/api/libdrvapi.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdrvapi.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdrvapi.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdrvapi.so"
         OLD_RPATH "/usr/local/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdrvapi.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES
    "/users/alanandr/2/drv/api/DrvAPIAddress.hpp"
    "/users/alanandr/2/drv/api/DrvAPIAddressMap.hpp"
    "/users/alanandr/2/drv/api/DrvAPIAddressToNative.hpp"
    "/users/alanandr/2/drv/api/DrvAPIAllocator.hpp"
    "/users/alanandr/2/drv/api/DrvAPICoreXY.hpp"
    "/users/alanandr/2/drv/api/DrvAPIGlobal.hpp"
    "/users/alanandr/2/drv/api/DrvAPI.hpp"
    "/users/alanandr/2/drv/api/DrvAPIInfo.hpp"
    "/users/alanandr/2/drv/api/DrvAPIMain.hpp"
    "/users/alanandr/2/drv/api/DrvAPIMemory.hpp"
    "/users/alanandr/2/drv/api/DrvAPINativeToAddress.hpp"
    "/users/alanandr/2/drv/api/DrvAPIOp.hpp"
    "/users/alanandr/2/drv/api/DrvAPIPointer.hpp"
    "/users/alanandr/2/drv/api/DrvAPIReadModifyWrite.hpp"
    "/users/alanandr/2/drv/api/DrvAPIThreadState.hpp"
    "/users/alanandr/2/drv/api/DrvAPISysConfig.hpp"
    "/users/alanandr/2/drv/api/DrvAPISystem.hpp"
    "/users/alanandr/2/drv/api/DrvAPIThread.hpp"
    "/users/alanandr/2/drv/api/DrvAPIVar.hpp"
    "/users/alanandr/2/drv/api/DrvAPISection.hpp"
    "/users/alanandr/2/drv/api/DrvAPIBits.hpp"
    "/users/alanandr/2/drv/api/DrvAPIDMA.hpp"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/users/alanandr/2/drv/build/api/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
