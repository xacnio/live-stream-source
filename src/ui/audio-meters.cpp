// audio-meters.cpp
#include "ui/audio-meters.h"
#include "core/common.h"
#include "network/ws-stats-server.h"

#include <obs-audio-controls.h>
#include <obs.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace lss {

static constexpr int64_t LEASE_MS = 8000;
static constexpr float PUBLISH_INTERVAL_S = 0.1f;
static constexpr int SYNC_EVERY_ROUNDS = 5;
static constexpr float SILENCE_DB = -100.0f;

struct AudioMeters::Meter {
  obs_volmeter_t *volmeter = nullptr;
  std::atomic<float> peak_db{SILENCE_DB}; // loudest since the last report

  ~Meter() {
    if (volmeter)
      obs_volmeter_destroy(volmeter);
  }
};

// Audio thread.
static void on_levels(void *param, const float[MAX_AUDIO_CHANNELS],
                      const float peak[MAX_AUDIO_CHANNELS], const float[MAX_AUDIO_CHANNELS]) {
  auto *slot = static_cast<std::atomic<float> *>(param);
  float loudest = SILENCE_DB;
  for (int i = 0; i < MAX_AUDIO_CHANNELS; ++i) {
    if (std::isfinite(peak[i]))
      loudest = std::max(loudest, peak[i]);
  }
  float current = slot->load();
  while (loudest > current && !slot->compare_exchange_weak(current, loudest)) {
  }
}

AudioMeters &AudioMeters::instance() {
  static AudioMeters meters;
  return meters;
}

void AudioMeters::keep_alive() {
  lease_until_ms_ = now_ms() + LEASE_MS;
}

void AudioMeters::end() {
  lease_until_ms_ = 0;
}

void AudioMeters::stop() {
  end();
  meters_.clear();
}

void AudioMeters::tick(float seconds) {
  if (now_ms() > lease_until_ms_) {
    meters_.clear();
    elapsed_ = 0.0f;
    rounds_ = 0;
    return;
  }

  elapsed_ += seconds;
  if (elapsed_ < PUBLISH_INTERVAL_S)
    return;
  elapsed_ = 0.0f;

  if (rounds_++ % SYNC_EVERY_ROUNDS == 0)
    sync_meters();
  publish();
}

void AudioMeters::sync_meters() {
  std::set<std::string> wanted;

  auto visit = [&](obs_source_t *source) {
    // Same rule as the OBS mixer: audio sources that are in use right now.
    if (!(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) || !obs_source_active(source))
      return;
    const char *name = obs_source_get_name(source);
    if (!name)
      return;
    wanted.insert(name);
    if (meters_.count(name))
      return;

    auto meter = std::make_unique<Meter>();
    meter->volmeter = obs_volmeter_create(OBS_FADER_LOG);
    if (!meter->volmeter)
      return;
    obs_volmeter_add_callback(meter->volmeter, on_levels, &meter->peak_db);
    if (obs_volmeter_attach_source(meter->volmeter, source))
      meters_[name] = std::move(meter);
  };

  obs_enum_sources(
      [](void *param, obs_source_t *source) {
        (*static_cast<decltype(visit) *>(param))(source);
        return true;
      },
      &visit);

  for (auto it = meters_.begin(); it != meters_.end();) {
    if (wanted.count(it->first))
      ++it;
    else
      it = meters_.erase(it);
  }
}

void AudioMeters::publish() {
  if (meters_.empty())
    return;

  obs_data_t *levels = obs_data_create();
  for (auto &entry : meters_) {
    const float db = entry.second->peak_db.exchange(SILENCE_DB);
    obs_data_set_double(levels, entry.first.c_str(), std::round(db * 10.0f) / 10.0f);
  }

  obs_data_t *root = obs_data_create();
  obs_data_set_obj(root, "levels", levels);
  const char *json = obs_data_get_json(root);
  if (json)
    WsStatsServer::instance().send_text(json);

  obs_data_release(root);
  obs_data_release(levels);
}

} // namespace lss
