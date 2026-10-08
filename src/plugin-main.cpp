#include <obs-module.h>
#include <obs-frontend-api.h>

#include "audio-health.h"

OBS_DECLARE_MODULE()

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT)
		audio_health_shutdown();
}

extern "C" MODULE_EXPORT bool obs_module_load(void)
{
	blog(LOG_INFO, "[audio-health] plugin caricato");
	return true;
}

extern "C" MODULE_EXPORT void obs_module_post_load(void)
{
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	audio_health_create_dock();
}

extern "C" MODULE_EXPORT void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	audio_health_shutdown();
}
