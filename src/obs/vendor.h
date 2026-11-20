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

#include <obs-module.h>

/* What plays now and what comes next, where a controller can read it (R-UX-11): Playlister as an obs-websocket vendor,
 * "obs-playlister", with the request get_now_next and the event now_next_changed. README, "For controllers", has
 * the fields. obs-websocket ships with OBS; where it is absent or switched off nothing is registered and nothing is
 * sent, and the plugin works as before. No header of obs-websocket is needed: its vendor interface is a set of procs
 * on a proc handler it publishes, which this file calls by name. */

/* The id of the source type, which the vendor asks sources for */
#define PL_SOURCE_ID "obs_playlister_source"

/* Registers the vendor and its request. From obs_module_post_load, when every module has loaded and obs-websocket
 * has published its interface. */
void pl_vendor_load(void);
/* Forgets the vendor: nothing is sent from here on. From obs_module_unload. */
void pl_vendor_unload(void);

/* Sends now_next_changed for this source. The source's video_tick calls it when what it last told no longer holds;
 * it reads the source through its get_now_next proc. */
void pl_vendor_now_next_changed(obs_source_t *source);
