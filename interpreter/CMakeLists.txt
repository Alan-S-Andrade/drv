file(
  GLOB
  RISCV_INTERPRETER_SOURCES
  *.cpp
  )
file(
  GLOB
  RISCV_INTERPRETER_HEADERS
  *.hpp
  )
list(APPEND
  RISCV_INTERPRETER_SOURCES
  ${RISCV_INTERPRETER_HEADERS}
  )

add_library(
  riscvinterpreter
  SHARED
  ${RISCV_INTERPRETER_SOURCES}
  )

target_compile_options(
  riscvinterpreter
  PUBLIC
  ${CXX_STD}
  )

target_include_directories(
  riscvinterpreter
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  )

install(
  TARGETS
  riscvinterpreter
  LIBRARY DESTINATION lib
  )
