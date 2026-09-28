cmake_minimum_required(VERSION 3.16)
project(gea_sailfish LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
include(${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake)

add_executable(${GEA_PACKAGE_NAME} ${GEA_C_SOURCES} ${GEA_CXX_SOURCES})
target_include_directories(${GEA_PACKAGE_NAME} PRIVATE ${GEA_INCLUDE_DIRS})
target_compile_options(${GEA_PACKAGE_NAME} PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_SOURCE_DIR}/platform/include/rpios_prelude.h>
  -funwind-tables)
target_compile_definitions(${GEA_PACKAGE_NAME} PRIVATE
  GEA_EMBEDDED_HAS_GENERATED_FONTS=1
  $<$<COMPILE_LANGUAGE:C>:GEA_EMBEDDED_GIF_C_API>
  $<$<COMPILE_LANGUAGE:CXX>:GEA_SAILFISH_OS>
  GEA_RPIOS_APP_ID="${GEA_APP_ID}"
  GEA_RPIOS_APP_TITLE="${GEA_APP_TITLE}")
target_link_libraries(${GEA_PACKAGE_NAME} PRIVATE PkgConfig::SDL2 PkgConfig::CURL pthread m rt)
target_link_options(${GEA_PACKAGE_NAME} PRIVATE -Wl,--export-dynamic)

install(TARGETS ${GEA_PACKAGE_NAME} RUNTIME DESTINATION bin)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/${GEA_PACKAGE_NAME}.desktop DESTINATION share/applications)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/${GEA_PACKAGE_NAME}.png
  DESTINATION share/icons/hicolor/128x128/apps)
