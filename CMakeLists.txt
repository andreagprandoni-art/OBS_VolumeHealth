cmake_minimum_required(VERSION 3.16...3.26)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# Questo plugin usa sempre il pannello (frontend API) e Qt
find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

if(COMMAND find_qt)
  find_qt(COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt::Core Qt::Widgets)
else()
  find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
endif()

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

# I warning non devono bloccare la compilazione
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8;/WX-> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-error>)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp src/audio-health.cpp src/audio-health.h)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
