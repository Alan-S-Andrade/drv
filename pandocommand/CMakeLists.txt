if (NOT DEFINED ARCH_RV64)
  find_package(DRV REQUIRED)
  add_library(
    pandocommand
    SHARED
    control.cpp
    loader.cpp
    )

  target_include_directories(
    pandocommand
    PUBLIC
    ${PROJECT_SOURCE_DIR}
    )

  target_link_libraries(
    pandocommand
    PUBLIC
    drvapi
    )

  set(
    PANDOCOMMAND_HEADERS
    control.hpp
    debug.hpp
    executable.hpp
    loader.hpp
    place.hpp
    )

  set_target_properties(
    pandocommand
    PROPERTIES
    PUBLIC_HEADER
    "${PANDOCOMMAND_HEADERS}"
    )

  install(
    TARGETS pandocommand
    DESTINATION lib
    PUBLIC_HEADER DESTINATION include/pandocommand
    )

  drvx_add_executable(
    pandocommand_loader
    default.cpp
    )

  target_link_libraries(
    pandocommand_loader
    PUBLIC
    pandocommand
    )

  set_target_properties(
    pandocommand_loader
    PROPERTIES
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
    )

  install(
    TARGETS pandocommand_loader
    RUNTIME DESTINATION lib
    )
endif()
