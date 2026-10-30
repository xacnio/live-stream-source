// obs-remote.h
#pragma once

namespace lss {

// Sends the scene list, stream state and audio sources to panel clients, and
// accepts scene changes, stream start and stop, volume, mute and the program
// preview from them. OBS settings stay out of reach.
void init_obs_remote();
void shutdown_obs_remote();

} // namespace lss
