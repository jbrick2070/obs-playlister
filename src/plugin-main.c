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

#include "core/random.h"
#include "core/text.h"
#include "providers/provider.h"

#ifdef TEST_SHUFFLER
#include "shuffler.h"
#endif

OBS_DECLARE_MODULE()

extern struct obs_source_info media_playlist_source_info;

/* The module's texts, as OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US") defines them, written out because of
 * obs_module_free_locale: OBS calls it before obs_module_unload, and the workers of retired providers (R-THR-3),
 * which may still be finishing a call on a slow share, look texts up for their row status. So they are waited for
 * here, before the texts go, and the shared code's lookup is taken away with them. */
lookup_t *obs_module_lookup = NULL;

const char *obs_module_text(const char *val)
{
	const char *out = val;
	text_lookup_getstr(obs_module_lookup, val, &out);
	return out;
}

bool obs_module_get_string(const char *val, const char **out)
{
	return text_lookup_getstr(obs_module_lookup, val, out);
}

void obs_module_set_locale(const char *locale)
{
	if (obs_module_lookup)
		text_lookup_destroy(obs_module_lookup);
	obs_module_lookup = obs_module_load_locale(obs_current_module(), "en-US", locale);
}

void obs_module_free_locale(void)
{
	pl_provider_reaper_shutdown();
	pl_text_set_lookup(NULL);
	text_lookup_destroy(obs_module_lookup);
	obs_module_lookup = NULL;
}

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
	pl_provider_reaper_open();
	/* the shuffle's random numbers, seeded once here for every thread that draws (core/random.h) */
	pl_random_seed();
	obs_register_source(&media_playlist_source_info);
#ifdef TEST_SHUFFLER
	test_shuffler();
#endif
	return true;
}

void obs_module_unload(void)
{
	/* both done already in obs_module_free_locale, which OBS calls first; again here so the unload does not depend
	 * on that order. After it a provider retired late is joined by its caller (pl_provider_reaper_shutdown). */
	pl_provider_reaper_shutdown();
	pl_text_set_lookup(NULL);
	obs_log(LOG_INFO, "plugin unloaded");
}
