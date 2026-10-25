# obs-playlister CTest suite. Tests link OBS::libobs (util layer only) and run headless.
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later

# Directories holding libobs and its dependency DLLs, put on the test PATH so nothing is copied and nothing is taken
# from an installed OBS or a developer PATH (REQUIREMENTS R-TST-4).
set(_test_dll_dirs "")
foreach(_config IN ITEMS RELEASE RELWITHDEBINFO DEBUG NOCONFIG)
  get_target_property(_libobs_location OBS::libobs IMPORTED_LOCATION_${_config})
  if(_libobs_location)
    cmake_path(GET _libobs_location PARENT_PATH _libobs_dir)
    list(APPEND _test_dll_dirs "${_libobs_dir}")
    break()
  endif()
endforeach()
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
  if(EXISTS "${_prefix}/share/obs-deps/VERSION" AND EXISTS "${_prefix}/bin")
    list(APPEND _test_dll_dirs "${_prefix}/bin")
  endif()
endforeach()

set(_test_environment "")
foreach(_dir IN LISTS _test_dll_dirs)
  cmake_path(NATIVE_PATH _dir _native_dir)
  list(APPEND _test_environment "PATH=path_list_prepend:${_native_dir}")
endforeach()
message(STATUS "obs-playlister tests: DLL search dirs: ${_test_dll_dirs}")

# One executable per suite: links only OBS::libobs and runs with the DLL directories above on its PATH
function(obs_playlister_add_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(${name} PRIVATE OBS::libobs)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION "${_test_environment}")
endfunction()

# Upstream shuffler self-tests (shuffler.c TEST_SHUFFLER block) plus regression tests, see test-shuffler.c
obs_playlister_add_test(test-shuffler test-shuffler.c)
# The shuffler's guards as the plugin is built, with asserts compiled out (R-UP-74)
obs_playlister_add_test(test-shuffler-release test-shuffler-release.c)
# The player's command FIFO
obs_playlister_add_test(test-command-queue test-command-queue.c "${CMAKE_SOURCE_DIR}/src/core/command-queue.c")
# Which end of the child may auto-advance
obs_playlister_add_test(test-load-tracker test-load-tracker.c "${CMAKE_SOURCE_DIR}/src/core/load-tracker.c")
# The child's audio on its way from the capture callback to video_tick
obs_playlister_add_test(test-audio-queue test-audio-queue.c "${CMAKE_SOURCE_DIR}/src/core/audio-queue.c")
# Container entries: copies, natural order, the extension filter, display order, modes and occurrence keys
set(_core "${CMAKE_SOURCE_DIR}/src/core")
obs_playlister_add_test(test-entry test-entry.c "${_core}/entry-list.c")
obs_playlister_add_test(test-natural test-natural.c "${_core}/natural.c")
obs_playlister_add_test(test-filter test-filter.c "${_core}/filter.c")
obs_playlister_add_test(test-sort test-sort.c "${_core}/sort.c" "${_core}/natural.c" "${_core}/entry-list.c")
obs_playlister_add_test(test-modes test-modes.c "${_core}/modes.c" "${_core}/sort.c" "${_core}/natural.c")
obs_playlister_add_test(test-keys test-keys.c "${_core}/keys.c" "${_core}/sort.c" "${_core}/natural.c")

# Every locale key used in src/ is in en-US.ini (R-TST-1); the second test proves the check fails on a missing key
add_test(
  NAME check-locale-keys
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-keys.cmake"
)
add_test(
  NAME check-locale-keys-fails-on-missing-key
  COMMAND
    "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "-DEXTRA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/locale-missing-key.c" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-keys.cmake"
)
set_tests_properties(
  check-locale-keys-fails-on-missing-key
  PROPERTIES PASS_REGULAR_EXPRESSION "LocaleKeyThatIsNotInTheLocaleFile \\(.*locale-missing-key\\.c\\)"
)
