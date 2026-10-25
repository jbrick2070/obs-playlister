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

/* Row classification (R-LIST-1): the rules apply in order, and only the last one asks the disk */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>
#include <windows.h>

#include "core/row-kind.h"

/* A stand-in disk: these paths are directories, and every question is counted */
struct fake_disk {
	const char *const *directories;
	size_t count;
	int questions;
};

static bool fake_is_directory(const char *path, void *context)
{
	struct fake_disk *disk = context;
	disk->questions++;
	for (size_t i = 0; i < disk->count; i++) {
		if (strcmp(disk->directories[i], path) == 0)
			return true;
	}
	return false;
}

static void expect(const struct pl_filter *filter, const char *path, bool saved_folder, enum pl_row_kind kind,
		   int questions)
{
	static const char *const directories[] = {"C:/media/shows", "C:/media/season.2", "C:/media/clips.mp4",
						  "D:\\renders"};
	struct fake_disk disk = {directories, sizeof(directories) / sizeof(directories[0]), 0};
	enum pl_row_kind found = pl_classify_row(path, saved_folder, filter, fake_is_directory, &disk);
	if (found != kind || disk.questions != questions) {
		fprintf(stderr, "%s (saved folder %d): kind %d, %d disk questions; expected kind %d, %d\n", path,
			saved_folder, found, disk.questions, kind, questions);
		assert(false);
	}
}

static void test_order(void)
{
	struct pl_filter filter;
	pl_filter_parse(&filter, PL_FILTER_DEFAULT);

	/* URLs: never folders and never on disk, whatever was saved */
	expect(&filter, "https://example.com/show/ep1", false, PL_ROW_FILE, 0);
	expect(&filter, "rtmp://example.com/live", false, PL_ROW_FILE, 0);
	expect(&filter, "https://example.com/feed", true, PL_ROW_FILE, 0);

	/* a saved folder kind wins without a disk question, even for a name that looks like media or is absent now */
	expect(&filter, "C:/media/gone", true, PL_ROW_FOLDER, 0);
	expect(&filter, "C:/media/clips.mp4", true, PL_ROW_FOLDER, 0);

	/* a media or still extension in the filter is a file without a disk question, in any case */
	expect(&filter, "C:/media/ep1.mp4", false, PL_ROW_FILE, 0);
	expect(&filter, "C:/media/EP1.MKV", false, PL_ROW_FILE, 0);
	expect(&filter, "C:/media/poster.jpeg", false, PL_ROW_FILE, 0);
	/* so is a folder whose name ends in one, unless its kind was saved (R-LIST-1, rule 3) */
	expect(&filter, "C:/media/clips.mp4", false, PL_ROW_FILE, 0);

	/* anything else asks the disk once: a directory is a folder, anything else a file */
	expect(&filter, "C:/media/shows", false, PL_ROW_FOLDER, 1);
	expect(&filter, "C:/media/season.2", false, PL_ROW_FOLDER, 1);
	expect(&filter, "D:\\renders", false, PL_ROW_FOLDER, 1);
	expect(&filter, "C:/media/notes.xyz", false, PL_ROW_FILE, 1);
	expect(&filter, "C:/media/moved", false, PL_ROW_FILE, 1);
	/* a share is a folder without a disk question (its provider decides on its worker), unless the name is media */
	expect(&filter, "\\\\server\\share\\shows", false, PL_ROW_FOLDER, 0);
	expect(&filter, "//server/share/shows", false, PL_ROW_FOLDER, 0);
	expect(&filter, "\\\\?\\UNC\\server\\share\\shows", false, PL_ROW_FOLDER, 0);
	expect(&filter, "\\\\server\\share\\ep1.mp4", false, PL_ROW_FILE, 0);
	/* a prefixed local path is not a share */
	expect(&filter, "\\\\?\\C:\\media\\notes", false, PL_ROW_FILE, 1);
	/* a trailing separator leaves the last component without an extension, so the disk decides */
	expect(&filter, "C:/media/shows/", false, PL_ROW_FILE, 1);
	expect(&filter, "C:/media/clips.mp4\\", false, PL_ROW_FILE, 1);

	pl_filter_free(&filter);
}

static void test_empty_filter(void)
{
	/* an empty filter admits nothing, so even a media name goes to the disk */
	struct pl_filter filter;
	pl_filter_parse(&filter, "");
	expect(&filter, "C:/media/ep1.mp4", false, PL_ROW_FILE, 1);
	expect(&filter, "C:/media/clips.mp4", false, PL_ROW_FOLDER, 1);
	pl_filter_free(&filter);
}

static void test_unc(void)
{
	assert(pl_path_is_unc("\\\\server\\share"));
	assert(pl_path_is_unc("//server/share/x"));
	assert(pl_path_is_unc("\\/server\\share"));
	assert(pl_path_is_unc("\\\\?\\UNC\\server\\share"));
	assert(pl_path_is_unc("//?/unc/server/share"));
	assert(!pl_path_is_unc("\\\\?\\C:\\media"));
	assert(!pl_path_is_unc("\\\\.\\C:\\media"));
	assert(!pl_path_is_unc("\\\\?\\"));
	assert(!pl_path_is_unc("\\\\"));
	assert(!pl_path_is_unc("\\\\\\server"));
	assert(!pl_path_is_unc("C:\\media"));
	assert(!pl_path_is_unc("/media"));
	assert(!pl_path_is_unc(""));
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	long allocations = bnum_allocs();
	test_order();
	test_empty_filter();
	test_unc();
	assert(bnum_allocs() == allocations);
	printf("row kind tests: passed\n");
	return 0;
}
