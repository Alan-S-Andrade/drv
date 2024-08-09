if (DEFINED ARCH_RV64)
  set(PANDOHAMMER_HEADERS
    pandohammer/address.h
    pandohammer/atomic.h
    pandohammer/cpuinfo.h
    pandohammer/mmio.h
    pandohammer/staticdecl.h
    )

  set(PANDOHAMMER_SOURCES
    crt.S
    lock.c
    )

  add_library(
    pandohammer
    OBJECT
    ${PANDOHAMMER_SOURCES}
    ${PANDOHAMMER_HEADERS}
    )

  target_link_options(
    pandohammer
    PUBLIC
    -nostartfiles
    -T ${CMAKE_CURRENT_SOURCE_DIR}/bsg_link.ld
    )
  
  target_include_directories(
    pandohammer
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    )

  set_target_properties(
    pandohammer
    PROPERTIES
    PUBLIC_HEADER
    "${PANDOHAMMER_HEADERS}"
    )

  install(FILES
    ${PANDOHAMMER_SOURCES}
    DESTINATION lib
    )
  install(FILES
    ${PANDOHAMMER_HEADERS}
    DESTINATION include/pandohammer
    )
  install(FILES
    bsg_link.ld
    DESTINATION lib
    )
  
endif()
