if (NOT DEFINED ARCH_RV64)
  find_package(Boost REQUIRED COMPONENTS coroutine)  
  set(
    DRV_API_SOURCES
    DrvAPIAddressToNative.cpp
    DrvAPIAddressMap.cpp
    DrvAPIAllocator.cpp
    DrvAPINativeToAddress.cpp
    DrvAPISysConfig.cpp
    DrvAPIThread.cpp
    DrvAPIThreadState.cpp
    DrvAPISection.cpp
    )

  set(
    DRV_API_HEADERS
    DrvAPIAddress.hpp
    DrvAPIAddressMap.hpp
    DrvAPIAddressToNative.hpp
    DrvAPIAllocator.hpp
    DrvAPICoreXY.hpp
    DrvAPIGlobal.hpp
    DrvAPI.hpp
    DrvAPIInfo.hpp
    DrvAPIMain.hpp
    DrvAPIMemory.hpp
    DrvAPINativeToAddress.hpp
    DrvAPIOp.hpp
    DrvAPIPointer.hpp
    DrvAPIReadModifyWrite.hpp
    DrvAPIThreadState.hpp
    DrvAPISysConfig.hpp
    DrvAPISystem.hpp
    DrvAPIThread.hpp
    DrvAPIVar.hpp
    DrvAPISection.hpp
    )


  add_library(
    drvapi
    SHARED
    ${DRV_API_SOURCES}
    ${DRV_API_HEADERS}
    )

  target_include_directories(
    drvapi
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    )

  target_link_libraries(
    drvapi
    PUBLIC
    Boost::boost
    Boost::coroutine
    )

  target_compile_options(
    drvapi
    PUBLIC
    ${CXX_STD}
    )

  set_target_properties(
    drvapi
    PROPERTIES
    PUBLIC_HEADER
    "${DRV_API_HEADERS}"
    )

  install(TARGETS drvapi
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include
    )  
endif()
