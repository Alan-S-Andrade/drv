project(pandohammer_project)
cmake_minimum_required(VERSION 3.25.0)

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
  STATIC
  ${PANDOHAMMER_SOURCES}
  ${PANDOHAMMER_HEADERS}
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

install(TARGETS pandohammer
  LIBRARY DESTINATION lib
  PUBLIC_HEADER DESTINATION include/pandohammer)

install(FILES bsg_link.ld DESTINATION lib)
