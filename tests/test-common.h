/*
obs-playlister
Copyright (C) 2026 Jeffrey A. Brick

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

/* Test setup that differs by platform. On Windows a failed assert, a debug CRT report or a crash would open a dialog
 * and hang ctest; this makes it print to stderr and exit non-zero instead. Elsewhere all three already do. Every test's
 * main calls it first.
 *
 * pl_test_fopen opens a file (a UTF-8 path) with the test's own C runtime. A FILE from libobs's os_fopen belongs to
 * libobs's runtime: a Debug test runs on the debug runtime while libobs is a release build, so its descriptor is not
 * open there, and reading or writing it fails the debug CRT's "_osfile(fh) & FOPEN" check. */

#include <stdio.h>

#ifdef _WIN32
#include <stdlib.h>
#include <windows.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

static inline void pl_test_no_crash_dialogs(void)
{
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifdef _DEBUG
	/* the debug CRT's own checks (an invalid parameter, a bad file descriptor) report to stderr, not to a dialog */
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

static inline FILE *pl_test_fopen(const char *path, const char *mode)
{
	wchar_t wide_path[2048];
	wchar_t wide_mode[8];
	if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide_path, 2048) ||
	    !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wide_mode, 8))
		return NULL;
	return _wfopen(wide_path, wide_mode);
}
#else
static inline void pl_test_no_crash_dialogs(void) {}

static inline FILE *pl_test_fopen(const char *path, const char *mode)
{
	return fopen(path, mode);
}
#endif
