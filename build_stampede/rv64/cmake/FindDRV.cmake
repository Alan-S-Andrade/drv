set(GNU_RISCV_TOOLCHAIN_PREFIX /install)
set(SST_CORE_PREFIX /install)
set(SST_ELEMENTS_PREFIX /install)
set(DRV_INSTALL_PREFIX /work/.local/rv64)
set(DRV_SOURCE_DIR /work2/10238/vineeth_architect/stampede3/drv)
set(DRV_BINARY_DIR /work2/10238/vineeth_architect/stampede3/drv/build_stampede/rv64)
set(SST_DIR /install/lib/cmake/SST)

if (NOT DEFINED ARCH_RV64)
  find_package(Boost REQUIRED)
  find_package(SST REQUIRED)
  if (NOT TARGET drvapi)
    message(STATUS "Adding drvapi target")
    add_library(drvapi SHARED IMPORTED GLOBAL)
    set_target_properties(drvapi PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES /work/.local/rv64/include
      IMPORTED_LOCATION /work/.local/rv64/lib/libdrvapi.so
      INTERFACE_LINK_LIBRARIES Boost::boost
      INTERFACE_COMPILE_OPTIONS -std=c++17
      )
  endif()
  if (NOT TARGET Drv)
    message(STATUS "Adding Drv target")
    add_library(Drv SHARED IMPORTED GLOBAL)
    set_target_properties(Drv PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES /work/.local/rv64/include
      IMPORTED_LOCATION /work/.local/rv64/lib/libDrv.so
      INTERFACE_LINK_LIBRARIES "drvapi SST::SSTCore"
      )
  endif()
  if (NOT TARGET pandocommand)
    message(STATUS "Adding pandocommand target")
    add_library(pandocommand SHARED IMPORTED GLOBAL)
    set_target_properties(pandocommand PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES /work/.local/rv64/include
      IMPORTED_LOCATION /work/.local/rv64/lib/libpandocommand.so
      INTERFACE_LINK_LIBRARIES drvapi
      )
  endif()
  if (NOT TARGET pandocommand_loader)
    message(STATUS "Adding pandocommand_loader target")
    add_library(pandocommand_loader SHARED IMPORTED GLOBAL)
    set_target_properties(pandocommand_loader PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES /work/.local/rv64/include
      IMPORTED_LOCATION /work/.local/rv64/lib/libpandocommand_loader.so
      INTERFACE_LINK_LIBRARIES pandocommand
      )
  endif()
else()
  if (NOT TARGET pandohammer)
    message("Adding pandohammer target")
    file(GLOB_RECURSE PANDOHAMMER_C_SOURCES ${DRV_INSTALL_PREFIX}/rv64/lib/*.c)
    file(GLOB_RECURSE PANDOHAMMER_ASM_SOURCES ${DRV_INSTALL_PREFIX}/rv64/lib/*.S)
    add_library(pandohammer OBJECT ${DRV_INSTALL_PREFIX} ${PANDOHAMMER_C_SOURCES} ${PANDOHAMMER_ASM_SOURCES})
    #set_property(TARGET pandohammer PROPERTY IMPORTED_LOCATION ${DRV_INSTALL_PREFIX}/pandohammer/lib/libpandohammer.a)
    target_include_directories(pandohammer PUBLIC ${DRV_INSTALL_PREFIX}/rv64/include)
    #set_property(TARGET pandohammer PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${DRV_INSTALL_PREFIX}/rv64/include)
    target_link_options(pandohammer PUBLIC -nostartfiles -T ${DRV_INSTALL_PREFIX}/rv64/lib/bsg_link.ld)
    #set_property(TARGET pandohammer PROPERTY INTERFACE_LINK_OPTIONS -T ${DRV_INSTALL_PREFIX}/rv64/lib/bsg_link.ld)
    #set_property(TARGET pandohammer PROPERTY INTERFACE_COMPILE_OPTIONS -nostartfiles)
    target_compile_options(pandohammer PUBLIC -nostartfiles)
  endif()
endif()

include(DRVInclude)
