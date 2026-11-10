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
#include <string.h>

#include "core/text.h"

#ifdef TEST_SHUFFLER
#include "shuffler.h"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info media_playlist_source_info;

/* R-I18N-1: shared code's messages in OBS's language. obs_module_text gives back the key itself when the locale has
 * no text for it, which here means "none", so the English in the code is used. */
static const char *locale_text(const char *key)
{
	const char *text = obs_module_text(key);
	return text == key ? NULL : text;
}

/* Shown by the OBS 33 Plugin Manager when data/manifest.json gives no display name. Keep it in sync with
 * buildspec.json (displayName). */
const char *obs_module_name(void)
{
	return "Playlister";
}

/* R-UX-9: in OBS's language (libobs sets the module's locale when it opens the module, before anyone can ask). The
 * English, the same as en-US.ini and cmake/common/manifest.json.in (description), only if no locale is loaded. */
const char *obs_module_description(void)
{
	const char *text = obs_module_text("Description");
	return strcmp(text, "Description") != 0
		       ? text
		       : "Plays video, audio and image files, folders, playlist files and web addresses, one after another.";
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	pl_text_set_lookup(locale_text);
	obs_register_source(&media_playlist_source_info);
#ifdef TEST_SHUFFLER
	test_shuffler();
#endif
	return true;
}

void obs_module_unload(void)
{
	pl_text_set_lookup(NULL);
	obs_log(LOG_INFO, "plugin unloaded");
}
