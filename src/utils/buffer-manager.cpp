// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025

#include "buffer-manager.h"
#include "core/common.h"
#include <algorithm>

namespace lss {

// Constructor - Initialize with empty state
BufferManager::BufferManager()
	: state_{0, 0, 0, 0, 0}
{
	lss_log_debug("BufferManager created");
}

// Update buffer state with current metrics
void BufferManager::update(const BufferState &state)
{
	state_ = state;
	
	lss_log_debug("BufferManager::update: video_frames=%d, audio_frames=%d, "
	              "network_delay=%lld ms, drift=%lld ms, buffer_duration=%lld ms",
	              state_.video_frames_buffered, state_.audio_frames_buffered,
	              state_.network_delay_ms, state_.drift_from_live_ms,
	              state_.buffer_duration_ms);
}

// Should we start catch-up playback?
bool BufferManager::should_start_catchup() const
{
	// Check all conditions:
	// 1. Drift from live > 1000ms
	bool drift_sufficient = state_.drift_from_live_ms > MIN_DRIFT_FOR_CATCHUP_MS;
	
	// 2. Buffer check (now 0, handled natively by OBS async queue)
	bool buffer_sufficient = state_.video_frames_buffered >= MIN_BUFFER_FRAMES;
	
	// 3. Network is stable (unused for catchup decision, pacing naturally causes spikes)
	bool network_stable = state_.network_delay_ms < NETWORK_STABLE_THRESHOLD_MS;
	
	// If we have a backlog > 400ms, we MUST catch up to clear it.
	bool should_start = drift_sufficient;
	
	if (should_start) {
		lss_log_debug("BufferManager: Catch-up conditions met "
		              "(drift=%lld ms, buffer=%d frames, network_delay=%lld ms)",
		              state_.drift_from_live_ms, state_.video_frames_buffered,
		              state_.network_delay_ms);
	}
	
	return should_start;
}

// Calculate optimal tempo for current drift
double BufferManager::calculate_optimal_tempo() const
{
	// Formula: tempo = 1.0 + (drift_ms / 1000.0) / TARGET_RECOVERY_SEC
	// Example: 1500ms drift → 1.0 + (1.5 / 3.0) = 1.5x tempo → clamped to 1.5x
	// Example:  800ms drift → 1.0 + (0.8 / 3.0) = 1.27x tempo
	// Example: 1800ms drift → 1.0 + (1.8 / 3.0) = 1.6x tempo → capped at 1.6x
	
	double drift_seconds = state_.drift_from_live_ms / 1000.0;
	double tempo = 1.0 + (drift_seconds / TARGET_RECOVERY_SEC);
	
	// Clamp to valid range (1.05 - 1.6)
	tempo = std::clamp(tempo, MIN_CATCHUP_TEMPO, MAX_CATCHUP_TEMPO);
	
	lss_log_debug("BufferManager::calculate_optimal_tempo: drift=%lld ms → tempo=%.4fx",
	              state_.drift_from_live_ms, tempo);
	
	return tempo;
}

// Should we skip to live instead of catch-up?
bool BufferManager::should_skip_to_live() const
{
	bool should_skip = state_.drift_from_live_ms > MAX_DRIFT_FOR_CATCHUP_MS;

	if (should_skip) {
		lss_log_warn("BufferManager: Drift too large for catch-up, should skip to live "
		             "(drift=%lld ms, threshold=%lld ms)",
		             (long long)state_.drift_from_live_ms,
		             (long long)MAX_DRIFT_FOR_CATCHUP_MS);
	}

	return should_skip;
}

void BufferManager::mark_catchup_exit()
{
}

// Get current buffer state
const BufferState &BufferManager::get_state() const
{
	return state_;
}

} // namespace lss
