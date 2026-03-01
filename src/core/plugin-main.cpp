// plugin-main.cpp - OBS module entry point
#include "core/plugin-main.h"
#include "core/live-stream-source.h"
#include "core/plugin-settings.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("live-stream-source", "en-US")

MODULE_EXPORT const char *obs_module_description(void) {
  return "Live Stream Source - plays live "
         "RTMP/HTTP-FLV/RTSP/SRT/AmazonIVS etc. "
         "streams with near-zero buffer latency.";
}

bool obs_module_load(void) {
  lss::init_plugin_settings(); // Register Tools menu & Load Config
  lss::register_live_stream_source();
  blog(LOG_INFO, "[LSS] Plugin loaded (v%d.%d.%d)", 1, 0, 0);
  return true;
}

void obs_module_unload(void) { blog(LOG_INFO, "[LSS] Plugin unloaded"); }
