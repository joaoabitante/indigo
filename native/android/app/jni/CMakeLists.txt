# Biblioteca nativa do app Android: SDL3 (baixado pelo build em jni/SDL) + núcleos e app de native/src.
cmake_minimum_required(VERSION 3.20)
project(IndigoAndroid LANGUAGES C CXX)

add_subdirectory(SDL)

set(INDIGO_APP OFF CACHE BOOL "" FORCE)
set(INDIGO_TOOLS OFF CACHE BOOL "" FORCE)
set(INDIGO_LTO OFF CACHE BOOL "" FORCE)
set(NATIVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_subdirectory(${NATIVE_ROOT} indigo)

add_library(main SHARED
  ${NATIVE_ROOT}/src/app/main.cpp
  ${NATIVE_ROOT}/src/app/ui.cpp
  ${NATIVE_ROOT}/src/app/platform.cpp
  ${NATIVE_ROOT}/src/app/shelf.cpp
  ${NATIVE_ROOT}/src/app/cloud.cpp
  ${NATIVE_ROOT}/src/app/video.cpp
  ${NATIVE_ROOT}/src/app/text.cpp
)
include(${NATIVE_ROOT}/cmake/embed.cmake)
indigo_embed("${CMAKE_CURRENT_BINARY_DIR}/gen/fonts.h" indigo_font "${NATIVE_ROOT}/res/fonts/Poppins-Regular.ttf" "${NATIVE_ROOT}/res/fonts/Poppins-SemiBold.ttf")
target_include_directories(main PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen")
target_link_libraries(main PRIVATE indigo_core SDL3::SDL3)
file(STRINGS ${NATIVE_ROOT}/VERSION INDIGO_VERSION LIMIT_COUNT 1)
target_compile_definitions(main PRIVATE INDIGO_VERSION="${INDIGO_VERSION}")
