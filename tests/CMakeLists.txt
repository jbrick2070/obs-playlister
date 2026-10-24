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

# Upstream shuffler self-tests (shuffler.c TEST_SHUFFLER block) plus regression tests, see test-shuffler.c
add_executable(test-shuffler test-shuffler.c)
target_include_directories(test-shuffler PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test-shuffler PRIVATE OBS::libobs)
add_test(NAME test-shuffler COMMAND test-shuffler)
set_tests_properties(test-shuffler PROPERTIES ENVIRONMENT_MODIFICATION "${_test_environment}")
