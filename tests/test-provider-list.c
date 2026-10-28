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

/* The list provider (Phase 3a step 3): local playlist files read again when they change (D-12 b), remote lists
 * through a stand-in fetcher (resolution against the final URL, refresh on the interval, one fetch in flight with
 * requests coalesced, the last good list kept with an error, the interval changed in place), remote rows classified
 * by their first response (R-LIST-1), direct media and HLS rows that are never fetched again, the D-12 a filter,
 * the lock, the "no dates" note, and destroy during a fetch that hangs. Each wait has a generous limit, so a slow
 * machine only makes it slower. */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <windows.h>

#include "core/filter.h"
#include "providers/provider-kinds.h"

#define WAIT_MS 15000

/* ---- a stand-in fetcher ---- */

struct route {
	const char *url;
	enum pl_fetch_result result; /* what open returns */
	uint32_t status;
	const char *type;
	const char *final_url; /* NULL: the URL itself */
	const char *body;
	bool hang;         /* open waits for the stop event */
	DWORD delay_ms;    /* open takes this long */
	long opens, reads; /* counted */
};

struct pl_fetch {
	struct route *route;
	struct pl_fetch_info info;
};

static pthread_mutex_t routes_mutex;
static struct route routes[16];
static size_t route_count;
static volatile long fetches_open;

static struct route *add_route(const char *url, enum pl_fetch_result result, uint32_t status, const char *type,
			       const char *final_url, const char *body)
{
	pthread_mutex_lock(&routes_mutex);
	struct route *route = &routes[route_count++];
	memset(route, 0, sizeof(*route));
	route->url = url;
	route->result = result;
	route->status = status;
	route->type = type;
	route->final_url = final_url;
	route->body = body;
	pthread_mutex_unlock(&routes_mutex);
	return route;
}

static void set_route(struct route *route, enum pl_fetch_result result, uint32_t status, const char *body)
{
	pthread_mutex_lock(&routes_mutex);
	route->result = result;
	route->status = status;
	route->body = body;
	pthread_mutex_unlock(&routes_mutex);
}

static long count_of(volatile long *counter)
{
	pthread_mutex_lock(&routes_mutex);
	long value = *counter;
	pthread_mutex_unlock(&routes_mutex);
	return value;
}

static struct pl_fetch *stub_open(const char *url, const struct pl_fetch_options *options, enum pl_fetch_result *result)
{
	struct pl_fetch *fetch = bzalloc(sizeof(*fetch));
	os_atomic_inc_long(&fetches_open);
	assert(strncmp(options->user_agent, "obs-playlister/", 15) == 0);
	assert(options->deadline_ms == 60000 && options->max_redirects == 5 && options->stop_event);

	pthread_mutex_lock(&routes_mutex);
	struct route *route = NULL;
	for (size_t i = 0; i < route_count; i++) {
		if (strcmp(routes[i].url, url) == 0)
			route = &routes[i];
	}
	if (route)
		route->opens++;
	bool hang = route && route->hang;
	DWORD delay = route ? route->delay_ms : 0;
	pthread_mutex_unlock(&routes_mutex);

	if (hang) {
		/* as WinHTTP does: the stop event ends the wait at once */
		DWORD signaled = WaitForSingleObject(options->stop_event, 30000);
		assert(signaled == WAIT_OBJECT_0);
		*result = PL_FETCH_CANCELLED;
		return fetch;
	}
	if (delay)
		Sleep(delay);
	if (!route) {
		fetch->info.error = 12007; /* ERROR_WINHTTP_NAME_NOT_RESOLVED */
		*result = PL_FETCH_NETWORK_ERROR;
		return fetch;
	}

	pthread_mutex_lock(&routes_mutex);
	fetch->route = route;
	fetch->info.status = route->status;
	fetch->info.content_type = route->type ? bstrdup(route->type) : NULL;
	fetch->info.final_url = bstrdup(route->final_url ? route->final_url : url);
	*result = route->result;
	pthread_mutex_unlock(&routes_mutex);
	return fetch;
}

static const struct pl_fetch_info *stub_info(const struct pl_fetch *fetch)
{
	return &fetch->info;
}

static enum pl_fetch_result stub_read(struct pl_fetch *fetch, size_t limit, bool prefix, char **body, size_t *size)
{
	pthread_mutex_lock(&routes_mutex);
	fetch->route->reads++;
	const char *text = fetch->route->body ? fetch->route->body : "";
	pthread_mutex_unlock(&routes_mutex);
	size_t length = strlen(text);
	if (length > limit && !prefix)
		return PL_FETCH_TOO_LARGE;
	if (length > limit)
		length = limit;
	*body = bstrdup_n(text, length);
	*size = length;
	return PL_FETCH_OK;
}

static void stub_close(struct pl_fetch *fetch)
{
	bfree(fetch->info.content_type);
	bfree(fetch->info.final_url);
	bfree(fetch);
	os_atomic_dec_long(&fetches_open);
}

static const struct pl_list_fetch_api stub_api = {stub_open, stub_info, stub_read, stub_close};

/* ---- what the provider published ---- */

struct inbox {
	pthread_mutex_t mutex;
	HANDLE event;
	DARRAY(struct pl_snapshot *) snapshots;
	uint64_t last_generation;
	bool in_order;
};

static void on_publish(void *owner, struct pl_snapshot *snapshot)
{
	struct inbox *inbox = owner;
	pthread_mutex_lock(&inbox->mutex);
	if (snapshot->generation <= inbox->last_generation)
		inbox->in_order = false;
	inbox->last_generation = snapshot->generation;
	da_push_back(inbox->snapshots, &snapshot);
	pthread_mutex_unlock(&inbox->mutex);
	SetEvent(inbox->event);
}

static void inbox_init(struct inbox *inbox)
{
	memset(inbox, 0, sizeof(*inbox));
	pthread_mutex_init(&inbox->mutex, NULL);
	inbox->event = CreateEventW(NULL, FALSE, FALSE, NULL);
	inbox->in_order = true;
}

static void inbox_free(struct inbox *inbox)
{
	assert(inbox->in_order);
	for (size_t i = 0; i < inbox->snapshots.num; i++)
		pl_snapshot_free(inbox->snapshots.array[i]);
	da_free(inbox->snapshots);
	CloseHandle(inbox->event);
	pthread_mutex_destroy(&inbox->mutex);
}

static size_t inbox_count(struct inbox *inbox)
{
	pthread_mutex_lock(&inbox->mutex);
	size_t count = inbox->snapshots.num;
	pthread_mutex_unlock(&inbox->mutex);
	return count;
}

typedef bool (*snapshot_test)(const struct pl_snapshot *snapshot, const void *context);

/* The first snapshot published from position since on that passes test; fails the test after the limit */
static const struct pl_snapshot *wait_for(struct inbox *inbox, size_t since, snapshot_test test, const void *context,
					  const char *what)
{
	uint64_t limit = os_gettime_ns() + (uint64_t)WAIT_MS * 1000000;
	for (;;) {
		pthread_mutex_lock(&inbox->mutex);
		for (size_t i = since; i < inbox->snapshots.num; i++) {
			if (test(inbox->snapshots.array[i], context)) {
				const struct pl_snapshot *found = inbox->snapshots.array[i];
				pthread_mutex_unlock(&inbox->mutex);
				return found;
			}
		}
		size_t count = inbox->snapshots.num;
		const struct pl_snapshot *last = count ? inbox->snapshots.array[count - 1] : NULL;
		if (os_gettime_ns() > limit) {
			fprintf(stderr, "timed out waiting for: %s (%zu snapshots", what, count);
			if (last) {
				fprintf(stderr, "; last: %zu entries, level %d, \"%s\", stale %d", last->count,
					last->level, last->status_message ? last->status_message : "", last->stale);
				for (size_t i = 0; i < last->count; i++)
					fprintf(stderr, "\n  %s | %s", last->entries[i].location, last->entries[i].key);
			}
			fprintf(stderr, ")\n");
			pthread_mutex_unlock(&inbox->mutex);
			assert(false);
		}
		pthread_mutex_unlock(&inbox->mutex);
		WaitForSingleObject(inbox->event, 100);
	}
}

/* The visible locations in order, joined by "|" */
static char *visible(const struct pl_snapshot *snapshot)
{
	struct dstr text = {0};
	for (size_t i = 0; i < snapshot->visible_count; i++) {
		if (i)
			dstr_cat_ch(&text, '|');
		dstr_cat(&text, snapshot->entries[snapshot->visible[i]].location);
	}
	return text.array ? text.array : bstrdup("");
}

static bool shows(const struct pl_snapshot *snapshot, const void *context)
{
	char *text = visible(snapshot);
	bool same = strcmp(text, (const char *)context) == 0;
	bfree(text);
	return same;
}

static bool says(const struct pl_snapshot *snapshot, const void *context)
{
	return snapshot->status_message && strstr(snapshot->status_message, (const char *)context);
}

static bool is_error_stale(const struct pl_snapshot *snapshot, const void *context)
{
	return snapshot->level == PL_STATUS_ERROR && snapshot->stale && says(snapshot, context);
}

static bool is_clean(const struct pl_snapshot *snapshot, const void *context)
{
	UNUSED_PARAMETER(context);
	return snapshot->level != PL_STATUS_ERROR && !snapshot->stale;
}

static void default_options(struct pl_container_options *options)
{
	memset(options, 0, sizeof(*options));
	options->extension_filter = PL_FILTER_DEFAULT;
	options->sort_key = PL_SORT_LIST;
	options->mode = PL_MODE_FULL;
	options->newest_count = 5;
	options->folder_check_enabled = true;
	options->folder_check_interval_s = 1;
	options->remote_refresh_s = 1;
}

static struct pl_provider *start(enum pl_row_kind kind, const char *location,
				 const struct pl_container_options *options, struct inbox *inbox)
{
	struct pl_provider *provider = pl_provider_create(kind, location, "row", 7, options, on_publish, inbox);
	assert(provider);
	return provider;
}

/* ---- local playlist files ---- */

static char folder[MAX_PATH];

static void write_file(const char *name, const char *text)
{
	char path[MAX_PATH * 2];
	snprintf(path, sizeof(path), "%s\\%s", folder, name);
	FILE *file = os_fopen(path, "wb");
	assert(file);
	fwrite(text, 1, strlen(text), file);
	fclose(file);
}

static char *path_of(const char *name)
{
	struct dstr path = {0};
	dstr_printf(&path, "%s\\%s", folder, name);
	return path.array;
}

static void test_local(void)
{
	char temp[MAX_PATH];
	GetTempPathA(sizeof(temp), temp);
	snprintf(folder, sizeof(folder), "%splaylister-list-%lu", temp, (unsigned long)GetCurrentProcessId());
	CreateDirectoryA(folder, NULL);

	/* a filtered extension goes, an extensionless entry and a stream stay (D-12 a); a nested list is counted */
	write_file("list.m3u", "#EXTM3U\n#EXTINF:1,One\na.mp4\nnotes.txt\nradio\nlive.m3u8\ninner.m3u\n");
	char *list = path_of("list.m3u");
	struct pl_container_options options;
	default_options(&options);
	struct inbox inbox;
	inbox_init(&inbox);
	struct pl_provider *provider = start(PL_ROW_PLAYLIST, list, &options, &inbox);

	struct dstr expected = {0};
	dstr_printf(&expected, "%s\\a.mp4|%s\\radio|%s\\live.m3u8", folder, folder, folder);
	const struct pl_snapshot *first = wait_for(&inbox, 0, shows, expected.array, "the local list");
	assert(first->initial_scan_complete && first->level == PL_STATUS_WARNING);
	assert(says(first, "1 playlists inside the list skipped"));
	assert(strcmp(first->entries[first->visible[0]].title, "One") == 0);
	/* keys of local entries are folded as folder keys are (R-KEY-1) */
	char *key = first->entries[first->visible[0]].key;
	assert(strstr(key, "/A.MP4") && !strchr(key, '\\'));

	/* the file changes: read again on the folder check interval (D-12 b) */
	/* each marker is taken before its change, as the worker may publish before the next line runs */
	Sleep(50);
	size_t since = inbox_count(&inbox);
	write_file("list.m3u", "a.mp4\nb.mp3\n");
	dstr_printf(&expected, "%s\\a.mp4|%s\\b.mp3", folder, folder);
	wait_for(&inbox, since, shows, expected.array, "the edited local list");

	/* the file goes away: the last good list is kept and the row says why (R-BEH-3) */
	since = inbox_count(&inbox);
	DeleteFileA(list);
	const struct pl_snapshot *gone =
		wait_for(&inbox, since, is_error_stale, "playlist file not found", "a missing playlist file");
	assert(gone->count == 2 && gone->last_ok_unix_ms > 0);

	/* and comes back */
	since = inbox_count(&inbox);
	write_file("list.m3u", "c.mkv\n");
	dstr_printf(&expected, "%s\\c.mkv", folder);
	const struct pl_snapshot *back = wait_for(&inbox, since, shows, expected.array, "the restored local list");
	assert(back->level == PL_STATUS_OK && !back->stale);
	pl_provider_destroy(provider);
	inbox_free(&inbox);

	/* a PLS file, and dates: date sort is list order and the row says so (R-DIR-10) */
	write_file("radio.pls", "[playlist]\nFile2=two.mp3\nFile1=one.mp3\n");
	char *pls = path_of("radio.pls");
	options.sort_key = PL_SORT_DATE;
	options.folder_check_enabled = false;
	inbox_init(&inbox);
	provider = start(PL_ROW_PLAYLIST, pls, &options, &inbox);
	dstr_printf(&expected, "%s\\one.mp3|%s\\two.mp3", folder, folder);
	const struct pl_snapshot *dated = wait_for(&inbox, 0, shows, expected.array, "the PLS file");
	assert(dated->level == PL_STATUS_OK && says(dated, "no dates; list order"));
	options.mode = PL_MODE_NEWEST_ONLY;
	since = inbox_count(&inbox);
	pl_provider_set_options(provider, &options);
	dstr_printf(&expected, "%s\\two.mp3", folder);
	const struct pl_snapshot *newest = wait_for(&inbox, since, shows, expected.array, "newest only on a PLS");
	assert(newest->level == PL_STATUS_WARNING && says(newest, "newest means last in the list"));
	pl_provider_destroy(provider);
	inbox_free(&inbox);

	DeleteFileA(list);
	DeleteFileA(pls);
	RemoveDirectoryA(folder);
	bfree(list);
	bfree(pls);
	dstr_free(&expected);
}

/* ---- remote lists ---- */

static void test_remote_list(void)
{
	/* relative entries against the final URL after redirects (R-REM-1) */
	struct route *route = add_route("https://lists.example.com/show.m3u", PL_FETCH_OK, 200, "audio/x-mpegurl",
					"https://cdn.example.com/v2/show.m3u", "#EXTM3U\nep1.mp3\nep2.mp3\n");
	struct pl_container_options options;
	default_options(&options);
	options.remote_refresh_s = 3600;
	struct inbox inbox;
	inbox_init(&inbox);
	struct pl_provider *provider = start(PL_ROW_PLAYLIST, "https://lists.example.com/show.m3u", &options, &inbox);
	const char *two = "https://cdn.example.com/v2/ep1.mp3|https://cdn.example.com/v2/ep2.mp3";
	const struct pl_snapshot *first = wait_for(&inbox, 0, shows, two, "the remote list");
	assert(first->initial_scan_complete && first->level == PL_STATUS_OK && first->last_ok_unix_ms > 0);
	assert(strcmp(first->entries[0].key, "https://cdn.example.com/v2/ep1.mp3") == 0);
	assert(count_of(&route->opens) == 1);

	/* a failure keeps the last good list and says so (R-REM-4); 429 and 503 alike */
	set_route(route, PL_FETCH_HTTP_STATUS, 429, NULL);
	size_t since = inbox_count(&inbox);
	pl_provider_refresh_now(provider);
	const struct pl_snapshot *failed =
		wait_for(&inbox, since, is_error_stale, "the server answered HTTP 429", "a 429 keeping the list");
	assert(failed->count == 2);

	/* the server comes back with a changed list */
	set_route(route, PL_FETCH_OK, 200, "#EXTM3U\nep1.mp3\nep2.mp3\nep3.mp3\n");
	since = inbox_count(&inbox);
	pl_provider_refresh_now(provider);
	const struct pl_snapshot *back = wait_for(&inbox, since, is_clean, NULL, "the list back");
	assert(back->count == 3);

	/* the interval changes in place: the provider is not re-created, and fetches again soon (R-REM-4) */
	long opens = count_of(&route->opens);
	options.remote_refresh_s = 1;
	pl_provider_set_options(provider, &options);
	uint64_t limit = os_gettime_ns() + (uint64_t)WAIT_MS * 1000000;
	while (count_of(&route->opens) < opens + 2 && os_gettime_ns() < limit)
		Sleep(50);
	assert(count_of(&route->opens) >= opens + 2);
	options.remote_refresh_s = 3600;
	pl_provider_set_options(provider, &options);

	/* one fetch in flight: requests that come meanwhile are one more fetch (R-REM-4) */
	Sleep(1500); /* a fetch the 1 s interval started has finished */
	pthread_mutex_lock(&routes_mutex);
	route->delay_ms = 2000; /* long enough that every request below comes while it runs */
	pthread_mutex_unlock(&routes_mutex);
	opens = count_of(&route->opens);
	pl_provider_refresh_now(provider);
	/* the requests below must come while that fetch is under way */
	limit = os_gettime_ns() + (uint64_t)WAIT_MS * 1000000;
	while (count_of(&route->opens) == opens && os_gettime_ns() < limit)
		Sleep(5);
	for (int i = 0; i < 10; i++)
		pl_provider_refresh_now(provider);
	limit = os_gettime_ns() + (uint64_t)WAIT_MS * 1000000;
	while (count_of(&route->opens) < opens + 2 && os_gettime_ns() < limit)
		Sleep(20);
	Sleep(3000); /* the second fetch ends, and a third would have begun */
	long fetched = count_of(&route->opens) - opens;
	assert(fetched == 2);
	pthread_mutex_lock(&routes_mutex);
	route->delay_ms = 0;
	pthread_mutex_unlock(&routes_mutex);

	/* the lock (R-DIR-11) applies to lists as to folders */
	options.lock_list_at_start = true;
	pl_provider_set_options(provider, &options);
	Sleep(200);
	set_route(route, PL_FETCH_OK, 200, "ep9.mp3\n");
	since = inbox_count(&inbox);
	pl_provider_refresh_now(provider);
	limit = os_gettime_ns() + (uint64_t)WAIT_MS * 1000000;
	while (pl_provider_held_changes(provider) == 0 && os_gettime_ns() < limit)
		Sleep(50);
	assert(pl_provider_held_changes(provider) == 4); /* three gone, one new */
	pl_provider_release_held(provider);
	wait_for(&inbox, since, shows, "https://cdn.example.com/v2/ep9.mp3", "the released list");

	pl_provider_destroy(provider);
	inbox_free(&inbox);
}

static bool single_entry(const struct pl_snapshot *snapshot, const void *context)
{
	return snapshot->count == 1 && strcmp(snapshot->entries[0].location, (const char *)context) == 0;
}

static void test_classification(void)
{
	struct pl_container_options options;
	default_options(&options);
	struct inbox inbox;

	/* audio/mpeg: the row is one media entry, closed without reading and never fetched again */
	const char *media = "https://radio.example.com/live";
	struct route *route = add_route(media, PL_FETCH_OK, 200, "audio/mpeg", NULL, "unused");
	inbox_init(&inbox);
	struct pl_provider *provider = start(PL_ROW_REMOTE, media, &options, &inbox);
	const struct pl_snapshot *direct = wait_for(&inbox, 0, single_entry, media, "a direct media row");
	assert(direct->entries[0].kind == PL_ENTRY_MEDIA && direct->level == PL_STATUS_OK);
	Sleep(2500); /* more than two refresh intervals */
	assert(count_of(&route->opens) == 1 && count_of(&route->reads) == 0);
	pl_provider_destroy(provider);
	inbox_free(&inbox);

	/* a playlist content type, an extensionless URL: PLS kept for later refreshes */
	const char *pls = "https://radio.example.com/listen";
	route = add_route(pls, PL_FETCH_OK, 200, "audio/x-scpls", NULL, "[playlist]\nFile1=http://a/1\n");
	inbox_init(&inbox);
	provider = start(PL_ROW_REMOTE, pls, &options, &inbox);
	wait_for(&inbox, 0, shows, "http://a/1", "a PLS by content type");
	set_route(route, PL_FETCH_OK, 200, "[playlist]\nFile1=http://a/1\nFile2=http://a/2\n");
	wait_for(&inbox, 0, shows, "http://a/1|http://a/2", "the PLS refreshed as PLS");
	pl_provider_destroy(provider);
	inbox_free(&inbox);

	/* HLS: one stream entry at the row's own URL, not the final one, and not fetched again */
	const char *hls = "https://video.example.com/live";
	route = add_route(hls, PL_FETCH_OK, 200, "application/vnd.apple.mpegurl", "https://edge.example.com/x/live",
			  "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nlow.m3u8\n");
	inbox_init(&inbox);
	provider = start(PL_ROW_REMOTE, hls, &options, &inbox);
	const struct pl_snapshot *stream = wait_for(&inbox, 0, single_entry, hls, "an HLS row");
	assert(stream->entries[0].kind == PL_ENTRY_STREAM);
	Sleep(2500);
	assert(count_of(&route->opens) == 1);
	pl_provider_destroy(provider);
	inbox_free(&inbox);

	/* text/plain that sniffs as M3U, short enough to need one fetch */
	const char *sniffed = "https://files.example.com/list";
	route = add_route(sniffed, PL_FETCH_OK, 200, "text/plain", NULL, "\xEF\xBB\xBF\n#EXTM3U\nx.mp4\n");
	options.remote_refresh_s = 3600;
	inbox_init(&inbox);
	provider = start(PL_ROW_REMOTE, sniffed, &options, &inbox);
	wait_for(&inbox, 0, shows, "https://files.example.com/x.mp4", "a sniffed list");
	assert(count_of(&route->opens) == 1);
	pl_provider_destroy(provider);
	inbox_free(&inbox);

	/* a feed, a web page, something else, and a name that does not resolve: errors, no entries */
	struct {
		const char *url, *type, *body, *message;
	} errors[] = {
		{"https://pod.example.com/feed", "application/rss+xml", "<rss/>", "feeds come in a later version"},
		{"https://pod.example.com/page", "text/html", "<!doctype html><html></html>", "a web page"},
		{"https://pod.example.com/blob", "application/octet-stream", "\x01\x02",
		 "content type application/octet"},
		{"https://nowhere.example.com/", NULL, NULL, "the server name could not be found"},
	};
	for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
		if (errors[i].type)
			add_route(errors[i].url, PL_FETCH_OK, 200, errors[i].type, NULL, errors[i].body);
		inbox_init(&inbox);
		provider = start(PL_ROW_REMOTE, errors[i].url, &options, &inbox);
		const struct pl_snapshot *error = wait_for(&inbox, 0, says, errors[i].message, errors[i].url);
		assert(error->level == PL_STATUS_ERROR && error->count == 0 && !error->stale);
		assert(error->initial_scan_complete);
		pl_provider_destroy(provider);
		inbox_free(&inbox);
	}

	/* a list URL that later answers with a web page keeps its list rather than reading the page as links */
	const char *list = "https://lists.example.com/later.m3u";
	route = add_route(list, PL_FETCH_OK, 200, "audio/x-mpegurl", NULL, "a.mp3\n");
	inbox_init(&inbox);
	provider = start(PL_ROW_PLAYLIST, list, &options, &inbox);
	wait_for(&inbox, 0, shows, "https://lists.example.com/a.mp3", "the list before the page");
	pthread_mutex_lock(&routes_mutex);
	route->type = "text/html";
	route->body = "<html><a href=\"x.mp3\">x</a></html>";
	pthread_mutex_unlock(&routes_mutex);
	size_t since = inbox_count(&inbox);
	pl_provider_refresh_now(provider);
	const struct pl_snapshot *page = wait_for(&inbox, since, is_error_stale, "web page instead of the list", list);
	assert(page->count == 1);
	pl_provider_destroy(provider);
	inbox_free(&inbox);
}

static bool count_is(const struct pl_snapshot *snapshot, const void *context)
{
	return snapshot->count == *(const size_t *)context;
}

/* What the step 3 review found: the lock after a failed start, a local stream read again, a file caught empty while
 * an editor saves it, and a list address that turns into media */
static void test_review_cases(void)
{
	struct pl_container_options options;
	default_options(&options);
	struct inbox inbox;

	/* down at start with the lock on: the first good list is shown, not held as a change (R-DIR-11) */
	const char *down = "https://lists.example.com/down.m3u";
	struct route *route = add_route(down, PL_FETCH_HTTP_STATUS, 503, NULL, NULL, NULL);
	options.lock_list_at_start = true;
	options.remote_refresh_s = 3600;
	inbox_init(&inbox);
	struct pl_provider *provider = start(PL_ROW_PLAYLIST, down, &options, &inbox);
	wait_for(&inbox, 0, says, "HTTP 503", "a list down at start");
	set_route(route, PL_FETCH_OK, 200, "a.mp3\n");
	pthread_mutex_lock(&routes_mutex);
	route->type = "audio/x-mpegurl";
	pthread_mutex_unlock(&routes_mutex);
	size_t since = inbox_count(&inbox);
	pl_provider_refresh_now(provider);
	wait_for(&inbox, since, shows, "https://lists.example.com/a.mp3", "the first good list, not held");
	assert(pl_provider_held_changes(provider) == 0);
	/* from now on the lock holds changes */
	set_route(route, PL_FETCH_OK, 200, "a.mp3\nb.mp3\n");
	pl_provider_refresh_now(provider);
	uint64_t limit = os_gettime_ns() + (uint64_t)WAIT_MS * 1000000;
	while (pl_provider_held_changes(provider) == 0 && os_gettime_ns() < limit)
		Sleep(50);
	assert(pl_provider_held_changes(provider) == 1);

	/* a list address that now answers with media: an error, the body never read, the list kept */
	long reads = count_of(&route->reads);
	pthread_mutex_lock(&routes_mutex);
	route->type = "audio/mpeg";
	pthread_mutex_unlock(&routes_mutex);
	since = inbox_count(&inbox);
	pl_provider_refresh_now(provider);
	wait_for(&inbox, since, is_error_stale, "answers with media", "a list that turned into media");
	assert(count_of(&route->reads) == reads);
	pl_provider_destroy(provider);
	inbox_free(&inbox);

	/* a local HLS file is one stream, and read again when edited into a list (D-12 b) */
	char temp[MAX_PATH];
	GetTempPathA(sizeof(temp), temp);
	snprintf(folder, sizeof(folder), "%splaylister-review-%lu", temp, (unsigned long)GetCurrentProcessId());
	CreateDirectoryA(folder, NULL);
	write_file("live.m3u8", "#EXTM3U\n#EXT-X-TARGETDURATION:4\nseg.ts\n");
	char *live = path_of("live.m3u8");
	default_options(&options);
	inbox_init(&inbox);
	provider = start(PL_ROW_PLAYLIST, live, &options, &inbox);
	const struct pl_snapshot *stream = wait_for(&inbox, 0, shows, live, "a local HLS file");
	assert(stream->entries[0].kind == PL_ENTRY_STREAM);
	Sleep(50);
	write_file("live.m3u8", "x.mp4\n");
	struct dstr expected = {0};
	dstr_printf(&expected, "%s\\x.mp4", folder);
	wait_for(&inbox, 0, shows, expected.array, "the local HLS file edited into a list");
	pl_provider_destroy(provider);
	inbox_free(&inbox);

	/* caught empty while an editor saves: the list is kept for one read; empty twice, it is empty */
	write_file("saved.m3u", "a.mp4\nb.mp4\n");
	char *saved = path_of("saved.m3u");
	options.folder_check_enabled = false;
	options.folder_check_interval_s = 4;
	inbox_init(&inbox);
	provider = start(PL_ROW_PLAYLIST, saved, &options, &inbox);
	size_t two = 2, none = 0;
	wait_for(&inbox, 0, count_is, &two, "the saved list");
	Sleep(50);
	since = inbox_count(&inbox);
	write_file("saved.m3u", "");
	uint64_t emptied = os_gettime_ns();
	pl_provider_refresh_now(provider);
	/* the retry comes on the 4 s interval, though the check is off */
	wait_for(&inbox, since, count_is, &none, "the list emptied on the second read");
	assert((os_gettime_ns() - emptied) / 1000000 >= 3000);
	pl_provider_destroy(provider);
	inbox_free(&inbox);

	DeleteFileA(live);
	DeleteFileA(saved);
	RemoveDirectoryA(folder);
	bfree(live);
	bfree(saved);
	dstr_free(&expected);
}

static void test_destroy_while_fetching(void)
{
	/* the fetch hangs; destroy returns at once and nothing is published afterwards (R-THR-3, R-REM-5) */
	const char *url = "https://slow.example.com/list.m3u";
	struct route *route = add_route(url, PL_FETCH_OK, 200, "audio/x-mpegurl", NULL, "a.mp3\n");
	route->hang = true;
	struct pl_container_options options;
	default_options(&options);
	struct inbox inbox;
	inbox_init(&inbox);
	struct pl_provider *provider = start(PL_ROW_PLAYLIST, url, &options, &inbox);
	uint64_t limit = os_gettime_ns() + (uint64_t)WAIT_MS * 1000000;
	while (count_of(&route->opens) == 0 && os_gettime_ns() < limit)
		Sleep(20);
	assert(count_of(&route->opens) == 1);
	uint64_t begun = os_gettime_ns();
	pl_provider_destroy(provider);
	assert((os_gettime_ns() - begun) / 1000000 < 5000);
	assert(inbox_count(&inbox) == 0);
	assert(os_atomic_load_long(&fetches_open) == 0);
	inbox_free(&inbox);
}

int main(void)
{
	long allocations = bnum_allocs();
	pthread_mutex_init(&routes_mutex, NULL);
	pl_list_provider_use_fetch_api(&stub_api);

	/* a file row has no provider */
	struct pl_container_options options;
	default_options(&options);
	assert(!pl_provider_create(PL_ROW_FILE, "C:\\x.mp4", "row", 1, &options, on_publish, NULL));

	test_local();
	test_remote_list();
	test_classification();
	test_review_cases();
	test_destroy_while_fetching();

	assert(os_atomic_load_long(&fetches_open) == 0);
	pl_list_provider_use_fetch_api(NULL);
	pthread_mutex_destroy(&routes_mutex);
	assert(bnum_allocs() == allocations);
	printf("test-provider-list: all passed\n");
	return 0;
}
