cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
option(OBS_PLAYLISTER_BUILD_TESTS "Build the CTest suite" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/playlist.h
    src/obs/playlister-source.h
    src/obs/playlister-source.c
    src/obs/player.h
    src/obs/player.c
    src/shuffler.h
    src/shuffler.c
    src/core/audio-queue.h
    src/core/audio-queue.c
    src/core/command-queue.h
    src/core/command-queue.c
    src/core/entry.h
    src/core/entry-list.c
    src/core/filter.h
    src/core/filter.c
    src/core/folder-state.h
    src/core/folder-state.c
    src/core/keys.h
    src/core/keys.c
    src/core/list-parse.h
    src/core/list-parse.c
    src/core/load-tracker.h
    src/core/load-tracker.c
    src/core/merge.h
    src/core/merge.c
    src/core/modes.h
    src/core/modes.c
    src/core/natural.h
    src/core/natural.c
    src/core/parse-m3u.h
    src/core/parse-m3u.c
    src/core/parse-pls.h
    src/core/parse-pls.c
    src/core/row-kind.h
    src/core/row-kind.c
    src/core/sort.h
    src/core/sort.c
    src/core/startup.h
    src/core/startup.c
    src/core/url.h
    src/core/url.c
    src/providers/fold-key-win.h
    src/providers/fold-key-win.c
    src/providers/provider.h
    src/providers/provider.c
    src/providers/provider-folder-win.c
)
# sources in src/obs/ include the shared headers by their path under src/ (core/..., shuffler.h, playlist.h)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(OBS_PLAYLISTER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
