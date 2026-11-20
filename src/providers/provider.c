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

#include "provider-kinds.h"

#include <util/bmem.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>

#include "os.h"
#include "publication.h"

void pl_snapshot_free(struct pl_snapshot *snapshot)
{
	if (!snapshot)
		return;
	pl_entries_free(snapshot->entries, snapshot->count);
	bfree(snapshot->visible);
	for (size_t i = 0; i < snapshot->pending_count; i++)
		bfree(snapshot->pending_keys[i]);
	bfree(snapshot->pending_keys);
	bfree(snapshot->status_message);
	bfree(snapshot);
}

/* ---- a row whose kind is not known yet (PL_ROW_UNKNOWN) ---- */

/* Asks the disk, on a worker of its own, whether the row's path is a directory, and publishes the answer once. The
 * question can take as long as a drive that is spinning up or a share that has gone quiet takes to answer, which is
 * why the source does not ask it on the graphics thread. On Windows the join cancels the call (os.h). */
struct pl_probe_provider {
	char *location;
	struct pl_publication publication; /* for the instance, the receiver and the cut-off (detach) */
	struct pl_stop *stop;
	struct pl_thread *thread;
};

static void probe_main(void *data)
{
	struct pl_probe_provider *p = data;
	os_set_thread_name("obs-playlister: row kind");
	if (pl_stop_is_set(p->stop))
		return;
	bool directory = pl_folder_is_directory(p->location);
	/* an answer that came because the call was cancelled is none */
	if (!pl_stop_is_set(p->stop))
		pl_publication_publish_probe(&p->publication, directory);
}

static void probe_free(struct pl_probe_provider *p)
{
	pl_stop_destroy(p->stop);
	pl_publication_free(&p->publication);
	bfree(p->location);
	bfree(p);
}

static struct pl_probe_provider *probe_create(const char *location, uint64_t instance_id, pl_publish_fn publish,
					      void *owner)
{
	if (!location || !publish)
		return NULL;
	struct pl_probe_provider *p = bzalloc(sizeof(*p));
	p->location = bstrdup(location);
	pl_publication_init(&p->publication, instance_id, publish, owner);
	p->stop = pl_stop_create();
	if (p->stop)
		p->thread = pl_thread_start(probe_main, p, NULL);
	if (!p->thread) {
		probe_free(p);
		return NULL;
	}
	return p;
}

static void probe_destroy(struct pl_probe_provider *p)
{
	pl_stop_set(p->stop);
	pl_thread_join(p->thread, p->location);
	probe_free(p);
}

/* ---- the kinds behind one interface ---- */

struct pl_provider {
	struct pl_folder_provider *folder;
	struct pl_list_provider *list;
	struct pl_probe_provider *probe;
};

struct pl_provider *pl_provider_create(enum pl_row_kind kind, const char *location, const char *row_uuid,
				       uint64_t instance_id, const struct pl_container_options *options,
				       pl_publish_fn publish, void *owner)
{
	struct pl_provider *provider = bzalloc(sizeof(*provider));
	if (kind == PL_ROW_FOLDER)
		provider->folder = pl_folder_provider_create(location, row_uuid, instance_id, options, publish, owner);
	else if (kind == PL_ROW_PLAYLIST || kind == PL_ROW_REMOTE)
		provider->list = pl_list_provider_create(kind, location, instance_id, options, publish, owner);
	else if (kind == PL_ROW_UNKNOWN)
		provider->probe = probe_create(location, instance_id, publish, owner);
	if (!provider->folder && !provider->list && !provider->probe) {
		bfree(provider);
		return NULL;
	}
	return provider;
}

/* a row of unknown kind has no list: nothing to list again, no options, nothing kept */
void pl_provider_refresh_now(struct pl_provider *provider)
{
	if (provider->folder)
		pl_folder_provider_refresh_now(provider->folder);
	else if (provider->list)
		pl_list_provider_refresh_now(provider->list);
}

void pl_provider_set_options(struct pl_provider *provider, const struct pl_container_options *options)
{
	if (provider->folder)
		pl_folder_provider_set_options(provider->folder, options);
	else if (provider->list)
		pl_list_provider_set_options(provider->list, options);
}

void pl_provider_release_held(struct pl_provider *provider)
{
	if (provider->folder)
		pl_folder_provider_release_held(provider->folder);
	else if (provider->list)
		pl_list_provider_release_held(provider->list);
}

size_t pl_provider_held_changes(struct pl_provider *provider)
{
	if (provider->folder)
		return pl_folder_provider_held_changes(provider->folder);
	return provider->list ? pl_list_provider_held_changes(provider->list) : 0;
}

void pl_provider_destroy(struct pl_provider *provider)
{
	if (!provider)
		return;
	if (provider->folder)
		pl_folder_provider_destroy(provider->folder);
	else if (provider->list)
		pl_list_provider_destroy(provider->list);
	else
		probe_destroy(provider->probe);
	bfree(provider);
}

void pl_provider_request_stop(struct pl_provider *provider)
{
	if (!provider)
		return;
	if (provider->folder)
		pl_folder_provider_request_stop(provider->folder);
	else if (provider->list)
		pl_list_provider_request_stop(provider->list);
	else
		pl_stop_set(provider->probe->stop);
}

/* ---- retired providers (R-THR-3) ---- */

/* One thread for the whole module joins and frees the providers handed to it, so neither the graphics thread (a list
 * edit) nor a source's destroy waits for a worker that is inside a call on a slow or stalled share. It starts with
 * the first retired provider and ends in pl_provider_reaper_shutdown. */
static pthread_mutex_t reaper_mutex = PTHREAD_MUTEX_INITIALIZER;
/* under reaper_mutex */
static DARRAY(struct pl_provider *) reaper_queue;
static struct pl_thread *reaper_thread;
static struct pl_stop *reaper_stop; /* set by the shutdown: the thread ends once the queue is empty */
static struct pl_wake *reaper_wake; /* a provider was queued */
static bool reaper_ending;
/* the shutdown ran (the module is unloading): no thread is started any more, and a provider retired from here on is
 * joined by its caller. A source something still holds is destroyed after the unload, and a thread started for it
 * would never be ended. */
static bool reaper_closed;

static void reaper_main(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("obs-playlister: retired");
	for (;;) {
		DARRAY(struct pl_provider *) batch;
		pthread_mutex_lock(&reaper_mutex);
		batch.da = reaper_queue.da;
		da_init(reaper_queue);
		bool ending = reaper_ending;
		pthread_mutex_unlock(&reaper_mutex);

		/* every one was asked to stop when it was retired, so they end side by side while this joins them in turn */
		size_t taken = batch.num;
		for (size_t i = 0; i < batch.num; i++)
			pl_provider_destroy(batch.array[i]);
		da_free(batch);
		if (taken)
			continue; /* more may have come meanwhile */
		if (ending)
			break;
		pl_wait(reaper_stop, reaper_wake, PL_WAIT_FOREVER);
	}
}

void pl_provider_retire(struct pl_provider *provider)
{
	if (!provider)
		return;
	pl_provider_request_stop(provider);
	if (provider->folder)
		pl_folder_provider_detach(provider->folder);
	else if (provider->list)
		pl_list_provider_detach(provider->list);
	else
		pl_publication_detach(&provider->probe->publication);

	bool queued = false;
	pthread_mutex_lock(&reaper_mutex);
	if (!reaper_thread && !reaper_ending && !reaper_closed) {
		if (!reaper_stop)
			reaper_stop = pl_stop_create();
		if (!reaper_wake)
			reaper_wake = pl_wake_create();
		if (reaper_stop && reaper_wake)
			reaper_thread = pl_thread_start(reaper_main, NULL, NULL);
	}
	if (reaper_thread && !reaper_ending) {
		da_push_back(reaper_queue, &provider);
		pl_wake_set(reaper_wake);
		queued = true;
	}
	pthread_mutex_unlock(&reaper_mutex);
	if (!queued)
		pl_provider_destroy(provider);
}

void pl_provider_reaper_open(void)
{
	pthread_mutex_lock(&reaper_mutex);
	reaper_closed = false;
	pthread_mutex_unlock(&reaper_mutex);
}

void pl_provider_reaper_shutdown(void)
{
	pthread_mutex_lock(&reaper_mutex);
	reaper_closed = true;
	struct pl_thread *thread = reaper_thread;
	if (thread) {
		reaper_ending = true;
		pl_stop_set(reaper_stop);
	}
	pthread_mutex_unlock(&reaper_mutex);
	if (thread)
		pl_thread_join(thread, "the retired providers");

	DARRAY(struct pl_provider *) left;
	pthread_mutex_lock(&reaper_mutex);
	left.da = reaper_queue.da;
	da_init(reaper_queue);
	reaper_thread = NULL;
	pl_stop_destroy(reaper_stop);
	pl_wake_destroy(reaper_wake);
	reaper_stop = NULL;
	reaper_wake = NULL;
	reaper_ending = false;
	pthread_mutex_unlock(&reaper_mutex);
	/* none in practice: a provider retired while the thread was ending is destroyed by its caller */
	for (size_t i = 0; i < left.num; i++)
		pl_provider_destroy(left.array[i]);
	da_free(left);
}
