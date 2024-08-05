# core sources for the element
set(
  DRV_SOURCES
  DrvCustomStdMem.cpp
  DrvCustomStdMem.hpp
  DrvEvent.hpp
  DrvNopEvent.hpp
  DrvMemEvent.hpp
  DrvMemory.cpp
  DrvMemory.hpp
  DrvSelfLinkMemory.cpp
  DrvSelfLinkMemory.hpp
  DrvStats.hpp
  DrvStatsTable.hpp
  DrvStdMemory.cpp
  DrvStdMemory.hpp
  DrvSimpleMemory.cpp
  DrvSimpleMemory.hpp
  DrvSysConfig.hpp
  DrvSystem.cpp
  DrvSystem.hpp
  )

# add the native simulation sources
set(DRV_X_SOURCES
  DrvThread.cpp
  DrvThread.hpp
  DrvCore.cpp
  DrvCore.hpp
  )

list(APPEND DRV_SOURCES ${DRV_X_SOURCES})

# add the ramulator simulation sources
set(DRV_RAMULATOR_SOURCES
  DrvCustomRamulatorMem.cpp
  DrvCustomRamulatorMem.hpp
  )

if (SST_ENABLE_RAMULATOR)
  list(APPEND DRV_SOURCES ${DRV_RAMULATOR_SOURCES})
endif()

# add the riscv simulation sources
set(DRV_R_SOURCES
  SSTRISCVCore.cpp
  SSTRISCVCore.hpp
  SSTRISCVHart.hpp
  SSTRISCVSimulator.cpp
  SSTRISCVSimulator.hpp
  DrvNativeSimulationTranslator.cpp
  DrvNativeSimulationTranslator.hpp
  )

list(APPEND DRV_SOURCES ${DRV_R_SOURCES})

add_sst_library(Drv ${DRV_SOURCES})

target_link_libraries(Drv LINK_PUBLIC drvapi memHierarchy)
target_include_directories(Drv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(Drv PUBLIC ${SST_ELEMENTS_PREFIX}/include)
target_compile_options(Drv PRIVATE ${CXX_STD})

target_link_libraries(Drv LINK_PUBLIC riscvinterpreter)
target_include_directories(Drv PUBLIC ${GNU_RISCV_TOOLCHAIN_PREFIX})

add_custom_command(
  TARGET Drv
  POST_BUILD
  COMMAND SST::SSTRegister Drv Drv_LIBDIR=${CMAKE_CURRENT_BINARY_DIR}
  )

install(TARGETS Drv DESTINATION lib)
# run with install to register the Drv element
add_custom_target(install-drv-element
  COMMAND SST::SSTRegister Drv Drv_LIBDIR=${CMAKE_INSTALL_PREFIX}/lib
  DEPENDS Drv
  )


