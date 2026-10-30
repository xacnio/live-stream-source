// obs-remote.cpp - what the web panel can see and do at the OBS level
#include "ui/obs-remote.h"
#include "ui/audio-meters.h"
#include "core/common.h"
#include "network/program-preview.h"
#include "network/ws-stats-server.h"
#include "playout/sync-registry.h"
#include "ui/sync-notice.h"

#include <obs-frontend-api.h>

#include <algorithm>
#include <string>

namespace lss {

static constexpr float AUDIO_PUBLISH_INTERVAL_S = 0.5f;
static constexpr int AUDIO_RESEND_ROUNDS = 4;

static float g_audio_elapsed = 0.0f;
static int g_audio_rounds = 0;
static std::string g_audio_json;
static bool g_remote_stopped = false;

// Frontend calls belong on the UI thread. Events already arrive there.
static void publish_state() {
  obs_frontend_source_list scenes = {};
  obs_frontend_get_scenes(&scenes);

  obs_data_array_t *names = obs_data_array_create();
  for (size_t i = 0; i < scenes.sources.num; ++i) {
    obs_data_t *item = obs_data_create();
    obs_data_set_string(item, "name", obs_source_get_name(scenes.sources.array[i]));
    obs_data_array_push_back(names, item);
    obs_data_release(item);
  }
  obs_frontend_source_list_free(&scenes);

  obs_source_t *current = obs_frontend_get_current_scene();

  obs_data_t *state = obs_data_create();
  obs_data_set_array(state, "scenes", names);
  obs_data_set_string(state, "current_scene",
                      current ? obs_source_get_name(current) : "");
  obs_data_set_bool(state, "streaming", obs_frontend_streaming_active());
  obs_data_set_bool(state, "recording", obs_frontend_recording_active());

  const char *json = obs_data_get_json(state);
  WsStatsServer::instance().set_obs_state(json ? json : "");

  obs_data_release(state);
  obs_data_array_release(names);
  obs_source_release(current);
}

static void publish_audio(void *, float seconds);

// Done when OBS says it's exiting, with graphics and audio still running. The steps
// are logged because a stuck shutdown says nothing else about where it stopped.
static void stop_remote() {
  if (g_remote_stopped)
    return;
  g_remote_stopped = true;

  WsStatsServer::instance().set_remote_handler(nullptr);
  lss_log_info("Shutdown: stopping program preview");
  ProgramPreview::instance().stop();
  lss_log_info("Shutdown: stopping audio");
  obs_remove_tick_callback(publish_audio, nullptr);
  AudioMeters::instance().stop();
  lss_log_info("Shutdown: stopping server");
  WsStatsServer::instance().release();
  lss_log_info("Shutdown: remote stopped");
}

static void on_frontend_event(enum obs_frontend_event event, void *) {
  switch (event) {
  case OBS_FRONTEND_EVENT_EXIT:
    stop_remote();
    break;
  case OBS_FRONTEND_EVENT_FINISHED_LOADING:
  case OBS_FRONTEND_EVENT_SCENE_CHANGED:
  case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
  case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
  case OBS_FRONTEND_EVENT_STREAMING_STARTED:
  case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
  case OBS_FRONTEND_EVENT_RECORDING_STARTED:
  case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
    publish_state();
    break;
  default:
    break;
  }
}

static bool add_audio_source(void *param, obs_source_t *source) {
  // Same rule as the OBS mixer: audio sources that are in use right now.
  if (!(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) || !obs_source_active(source))
    return true;

  obs_data_t *item = obs_data_create();
  obs_data_set_string(item, "name", obs_source_get_name(source));
  obs_data_set_double(item, "volume", obs_source_get_volume(source));
  obs_data_set_bool(item, "muted", obs_source_muted(source));
  obs_data_array_push_back(static_cast<obs_data_array_t *>(param), item);
  obs_data_release(item);
  return true;
}

static void publish_audio(void *, float seconds) {
  AudioMeters::instance().tick(seconds);

  g_audio_elapsed += seconds;
  if (g_audio_elapsed < AUDIO_PUBLISH_INTERVAL_S)
    return;
  g_audio_elapsed = 0.0f;

  if (WsStatsServer::instance().get_client_count() == 0)
    return;

  obs_data_array_t *sources = obs_data_array_create();
  obs_enum_sources(add_audio_source, sources);

  obs_data_t *state = obs_data_create();
  obs_data_set_array(state, "sources", sources);

  // Sent now and then even when nothing changed. With every stream idle there
  // are no stats either, and the panel would take the silence for a dead link.
  const char *json = obs_data_get_json(state);
  if (json && (g_audio_json != json || ++g_audio_rounds >= AUDIO_RESEND_ROUNDS)) {
    g_audio_rounds = 0;
    g_audio_json = json;
    WsStatsServer::instance().set_audio_state(g_audio_json);
  }

  obs_data_release(state);
  obs_data_array_release(sources);
}

static void set_scene_task(void *param) {
  auto *name = static_cast<std::string *>(param);

  obs_source_t *source = obs_get_source_by_name(name->c_str());
  // The name comes off the network: only ever act on an actual scene.
  if (source && obs_scene_from_source(source)) {
    lss_log_info("Remote: switching to scene '%s'", name->c_str());
    obs_frontend_set_current_scene(source);
  } else {
    lss_log_warn("Remote: no scene named '%s'", name->c_str());
  }
  obs_source_release(source);
  delete name;
}

static void start_stream_task(void *) {
  lss_log_info("Remote: starting the stream");
  obs_frontend_streaming_start();
}

static void stop_stream_task(void *) {
  lss_log_info("Remote: stopping the stream");
  obs_frontend_streaming_stop();
}

static void apply_audio(const RemoteCommand &command) {
  obs_source_t *source = obs_get_source_by_name(command.name.c_str());
  // Same as scenes: the name comes off the network.
  if (source && (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) {
    if (command.command == "set_mute")
      obs_source_set_muted(source, command.muted);
    else
      obs_source_set_volume(source, static_cast<float>(std::clamp(command.value, 0.0, 1.0)));
  } else {
    lss_log_warn("Remote: no audio source named '%s'", command.name.c_str());
  }
  obs_source_release(source);
}

static void handle_remote(const RemoteCommand &command) {
  if (command.command == "set_scene") {
    if (!command.name.empty())
      obs_queue_task(OBS_TASK_UI, set_scene_task, new std::string(command.name), false);
  } else if (command.command == "start_stream") {
    obs_queue_task(OBS_TASK_UI, start_stream_task, nullptr, false);
  } else if (command.command == "stop_stream") {
    obs_queue_task(OBS_TASK_UI, stop_stream_task, nullptr, false);
  } else if (command.command == "set_volume" || command.command == "set_mute") {
    apply_audio(command);
  } else if (command.command == "preview_program_on") {
    ProgramPreview::instance().keep_alive();
  } else if (command.command == "preview_program_off") {
    ProgramPreview::instance().end();
  } else if (command.command == "sync_match") {
    if (!SyncRegistry::instance().start_sync()) {
      show_status_notice(obs_module_text("SyncListenNoSource"));
      WsStatsServer::instance().send_text("{\"sync_error\":true}");
    }
  } else if (command.command == "audio_levels_on") {
    AudioMeters::instance().keep_alive();
  } else if (command.command == "audio_levels_off") {
    AudioMeters::instance().end();
  }
}

void init_obs_remote() {
  // The panel has to work with no Live Stream Source in the scene collection.
  WsStatsServer::instance().add_ref();
  obs_frontend_add_event_callback(on_frontend_event, nullptr);
  obs_add_tick_callback(publish_audio, nullptr);
  ProgramPreview::instance().start();
  WsStatsServer::instance().set_remote_handler(handle_remote);
}

void shutdown_obs_remote() {
  obs_frontend_remove_event_callback(on_frontend_event, nullptr);
  stop_remote();
}

} // namespace lss
