/*
obs-playlister (based on Media Playlist Source)
Copyright (C) 2023 Ian Rodriguez ianlemuelr@gmail.com
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

#include <obs-module.h>
#include <plugin-support.h>

#ifdef TEST_SHUFFLER
#include "shuffler.h"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info media_playlist_source_info;

/* Shown by the OBS 33 Plugin Manager when data/manifest.json gives no display name. Keep both strings in sync
 * with buildspec.json (displayName) and cmake/common/manifest.json.in (description). */
const char *obs_module_name(void)
{
	return "OBS Playlister";
}

const char *obs_module_description(void)
{
	return "Playlist source for OBS Studio: plays files and folders through OBS's media source.";
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	obs_register_source(&media_playlist_source_info);
#ifdef TEST_SHUFFLER
	test_shuffler();
#endif
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
