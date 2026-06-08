/*
 * obs-premiere-patch
 * Copyright (C) 2026 CIsaa
 *
 * After recording stops, reads chapter markers from the MP4 and
 * injects Premiere Pro-compatible XMP markers directly into the file.
 * Requires exiftool in PATH (winget install OliverBetz.ExifTool).
 *
 * GPL-2.0 - see LICENSE
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include "marker-patch.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static void on_frontend_event(enum obs_frontend_event event, void *unused)
{
	(void)unused;
	if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED)
		mp_on_recording_stopped();
	else if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		mp_on_obs_loaded();
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "[obs-premiere-patch] module load");
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	mp_setup_menu();
	mp_start();
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	mp_stop();
	obs_log(LOG_INFO, "[obs-premiere-patch] module unload");
}
