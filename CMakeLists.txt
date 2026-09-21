cmake_minimum_required(VERSION 3.25)

project(NanoSVG VERSION 2.0.0 LANGUAGES C CXX)
include(GNUInstallDirs)
option(NANOSVG_ALL_COLOR_KEYWORDS "Enable all SVG color keywords" OFF)

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/nanosvg.cpp"
    CONTENT "#define NANOSVG_IMPLEMENTATION\n#include \"nanosvg.h\"\n")
add_library(nanosvg "${CMAKE_CURRENT_BINARY_DIR}/nanosvg.cpp")
add_library(NanoSVG::nanosvg ALIAS nanosvg)
target_compile_features(nanosvg PUBLIC cxx_std_23)
find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
    target_link_libraries(nanosvg PUBLIC ${MATH_LIBRARY})
endif()

target_include_directories(nanosvg PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/nanosvg>"
)
target_compile_definitions(nanosvg PRIVATE
    $<$<BOOL:${NANOSVG_ALL_COLOR_KEYWORDS}>:NANOSVG_ALL_COLOR_KEYWORDS>)

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/nanosvgrast.cpp"
    CONTENT "#define NANOSVGRAST_IMPLEMENTATION\n#include \"nanosvgrast.h\"\n")
add_library(nanosvgrast "${CMAKE_CURRENT_BINARY_DIR}/nanosvgrast.cpp")
add_library(NanoSVG::nanosvgrast ALIAS nanosvgrast)
target_link_libraries(nanosvgrast PUBLIC nanosvg)
set_target_properties(nanosvg nanosvgrast PROPERTIES
    VERSION ${PROJECT_VERSION} SOVERSION 2
    CXX_EXTENSIONS OFF WINDOWS_EXPORT_ALL_SYMBOLS ON)

# Installation and export:

include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(TARGETS nanosvg nanosvgrast
        EXPORT ${PROJECT_NAME}Targets
)

export(EXPORT ${PROJECT_NAME}Targets 
       FILE "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Targets.cmake" 
       NAMESPACE ${PROJECT_NAME}::
)

set(ConfigPackageLocation ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME})

configure_package_config_file(${CMAKE_CURRENT_SOURCE_DIR}/Config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
  INSTALL_DESTINATION ${ConfigPackageLocation}
  NO_CHECK_REQUIRED_COMPONENTS_MACRO
)

install(
    FILES
      src/nanosvg.h
      src/nanosvgrast.h
      src/nanosvg.hpp
      src/nanosvgrast.hpp
    DESTINATION
      ${CMAKE_INSTALL_INCLUDEDIR}/nanosvg
  )

install(EXPORT ${PROJECT_NAME}Targets
  FILE
    ${PROJECT_NAME}Targets.cmake
  NAMESPACE
    ${PROJECT_NAME}::
  DESTINATION
    ${ConfigPackageLocation}
)

install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
  DESTINATION
    ${ConfigPackageLocation}
)
