// plugin-main.cpp - OBS module entry point
#include "core/plugin-main.h"
#include "core/live-stream-source.h"
#include "core/plugin-settings.h"
#include "ui/obs-remote.h"
#include "ui/context-buttons.h"
#include "ui/sync-panel.h"
#include "ui/update-checker.h"

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
  lss::FallbackLayer::register_filter();
  lss::init_update_checker();
  lss::init_obs_remote();
  lss::init_sync_panel();
  lss::init_context_buttons();
  blog(LOG_INFO, "[LSS] Plugin loaded (v" PLUGIN_VERSION ")");
  return true;
}

void obs_module_unload(void) {
  blog(LOG_INFO, "[LSS] Plugin unloading");
  lss::shutdown_context_buttons();
  lss::shutdown_sync_panel();
  lss::shutdown_obs_remote();
  blog(LOG_INFO, "[LSS] Stopping update checker");
  lss::shutdown_update_checker();
  blog(LOG_INFO, "[LSS] Plugin unloaded");
}
