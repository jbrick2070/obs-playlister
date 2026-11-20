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

#include "vendor.h"

#include <string.h>

#include <util/threading.h>
#include <plugin-support.h>

#define VENDOR_NAME "obs-playlister"
#define REQUEST_GET_NOW_NEXT "get_now_next"
#define EVENT_NOW_NEXT_CHANGED "now_next_changed"

/* obs-websocket's proc handler and this plugin's vendor on it; both NULL when obs-websocket is not there. Written once,
 * at load, before vendor_ready is set; the graphics thread reads them only while it is. */
static proc_handler_t *websocket_procs;
static void *vendor;
static volatile bool vendor_ready;

/* As obs-websocket takes a vendor request's callback: by a pointer to this, which it copies */
struct request_callback {
	void (*callback)(obs_data_t *request, obs_data_t *response, void *data);
	void *data;
};

static const char *text_of(calldata_t *cd, const char *name)
{
	const char *text = NULL;
	return calldata_get_string(cd, name, &text) && text ? text : "";
}

/* What one Playlister source plays now and next, into out; false when source is not one */
static bool describe_source(obs_source_t *source, obs_data_t *out)
{
	const char *id = source ? obs_source_get_unversioned_id(source) : NULL;
	if (!id || strcmp(id, PL_SOURCE_ID) != 0)
		return false;
	calldata_t cd = {0};
	if (!proc_handler_call(obs_source_get_proc_handler(source), "get_now_next", &cd)) {
		calldata_free(&cd);
		return false;
	}
	obs_data_set_string(out, "source", obs_source_get_name(source));
	obs_data_set_string(out, "source_uuid", obs_source_get_uuid(source));
	obs_data_set_string(out, "playing", text_of(&cd, "playing"));
	obs_data_set_string(out, "next", text_of(&cd, "next"));
	obs_data_set_bool(out, "next_decided", calldata_bool(&cd, "next_decided"));
	obs_data_set_string(out, "state", text_of(&cd, "state"));
	obs_data_set_string(out, "status", text_of(&cd, "status"));
	obs_data_set_int(out, "row", calldata_int(&cd, "row"));
	obs_data_set_int(out, "item", calldata_int(&cd, "item"));
	obs_data_set_int(out, "items", calldata_int(&cd, "items"));
	calldata_free(&cd);
	return true;
}

static bool add_source(void *data, obs_source_t *source)
{
	obs_data_array_t *sources = data;
	obs_data_t *one = obs_data_create();
	if (describe_source(source, one))
		obs_data_array_push_back(sources, one);
	obs_data_release(one);
	return true;
}

/* The request get_now_next. With "source_uuid" or "source" (a name): that source, and "found" says whether it is a
 * Playlister. With neither: "sources", every Playlister there is. On one of obs-websocket's threads. */
static void get_now_next_request(obs_data_t *request, obs_data_t *response, void *data)
{
	UNUSED_PARAMETER(data);
	const char *uuid = request ? obs_data_get_string(request, "source_uuid") : "";
	const char *name = request ? obs_data_get_string(request, "source") : "";
	if (*uuid || *name) {
		obs_source_t *source = *uuid ? obs_get_source_by_uuid(uuid) : obs_get_source_by_name(name);
		obs_data_set_bool(response, "found", describe_source(source, response));
		obs_source_release(source);
		return;
	}
	obs_data_array_t *sources = obs_data_array_create();
	obs_enum_sources(add_source, sources);
	obs_data_set_array(response, "sources", sources);
	obs_data_array_release(sources);
}

void pl_vendor_load(void)
{
	calldata_t cd = {0};
	/* obs-websocket adds this proc to the global handler when it loads; without it there is no obs-websocket */
	if (proc_handler_call(obs_get_proc_handler(), "obs_websocket_api_get_ph", &cd))
		websocket_procs = calldata_ptr(&cd, "ph");
	calldata_free(&cd);
	if (!websocket_procs) {
		obs_log(LOG_INFO, "obs-websocket is not there; what plays now and next is not published through it");
		return;
	}

	calldata_t registration = {0};
	calldata_set_string(&registration, "name", VENDOR_NAME);
	proc_handler_call(websocket_procs, "vendor_register", &registration);
	void *registered = calldata_ptr(&registration, "vendor");
	calldata_free(&registration);
	if (!registered) {
		obs_log(LOG_WARNING, "obs-websocket did not take the vendor %s", VENDOR_NAME);
		return;
	}

	struct request_callback callback = {get_now_next_request, NULL};
	calldata_t request = {0};
	calldata_set_ptr(&request, "vendor", registered);
	calldata_set_string(&request, "type", REQUEST_GET_NOW_NEXT);
	calldata_set_ptr(&request, "callback", &callback);
	proc_handler_call(websocket_procs, "vendor_request_register", &request);
	bool taken = calldata_bool(&request, "success");
	calldata_free(&request);
	if (!taken)
		obs_log(LOG_WARNING, "obs-websocket did not take the request %s", REQUEST_GET_NOW_NEXT);
	vendor = registered;
	os_atomic_set_bool(&vendor_ready, true);
	obs_log(LOG_INFO, "obs-websocket vendor %s: request %s, event %s", VENDOR_NAME, REQUEST_GET_NOW_NEXT,
		EVENT_NOW_NEXT_CHANGED);
}

void pl_vendor_unload(void)
{
	os_atomic_set_bool(&vendor_ready, false);
}

void pl_vendor_now_next_changed(obs_source_t *source)
{
	if (!os_atomic_load_bool(&vendor_ready))
		return;
	obs_data_t *event = obs_data_create();
	if (describe_source(source, event)) {
		calldata_t cd = {0};
		calldata_set_ptr(&cd, "vendor", vendor);
		calldata_set_string(&cd, "type", EVENT_NOW_NEXT_CHANGED);
		calldata_set_ptr(&cd, "data", event);
		/* obs-websocket copies the data into the event before this returns */
		proc_handler_call(websocket_procs, "vendor_event_emit", &cd);
		calldata_free(&cd);
	}
	obs_data_release(event);
}
