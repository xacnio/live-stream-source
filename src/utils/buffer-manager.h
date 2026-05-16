// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025

#pragma once

#include <cstdint>

namespace lss {

/**
 * @brief Buffer state information
 * 
 * Contains current buffer depth and network metrics used for
 * catch-up decision making.
 */
struct BufferState {
	int video_frames_buffered;   // Number of video frames in buffer
	int audio_frames_buffered;   // Number of audio frames in buffer
	int64_t network_delay_ms;    // Current network delay/jitter
	int64_t drift_from_live_ms;  // How far behind live edge (ms)
	int64_t buffer_duration_ms;  // Total buffer duration (ms)
};

/**
 * @brief Intelligent buffer monitoring and catch-up decision maker
 *
 * Monitors buffer depth and network conditions to make intelligent
 * decisions about when to start catch-up, what tempo to use, and
 * when to skip to live instead.
 *
 * Decision Logic:
 * - Start catch-up: drift > 800ms (headroom error > 600ms above 200ms target)
 * - Optimal tempo: 1.0 + (drift_ms / 1000.0) / 3.0 seconds, capped at 1.6x
 * - Skip to live: drift > 12000ms (12 seconds)
 */
class BufferManager {
public:
	BufferManager();

	/**
	 * @brief Update buffer state with current metrics
	 * @param state Current buffer and network state
	 */
	void update(const BufferState &state);

	/**
	 * @brief Should we start catch-up playback?
	 * @return true if conditions are met for catch-up
	 *
	 * Conditions:
	 * - Drift from live > 800ms (headroom error > 600ms)
	 * - Network delay within threshold
	 */
	bool should_start_catchup() const;

	/**
	 * @brief Calculate optimal tempo for current drift
	 * @return Tempo multiplier (1.05 - 1.6)
	 *
	 * Formula: tempo = 1.0 + (drift_ms / 1000.0) / TARGET_RECOVERY_SEC
	 * Target recovery time: 3 seconds
	 * Clamped to range: 1.05 - 1.6 (SoundTouch quality limit)
	 */
	double calculate_optimal_tempo() const;

	/**
	 * @brief Should we skip to live instead of catch-up?
	 * @return true if drift is too large for catch-up
	 *
	 * Returns true if drift > 12 seconds
	 */
	bool should_skip_to_live() const;

	/**
	 * @brief Get current buffer state
	 * @return Reference to current buffer state
	 */
	const BufferState &get_state() const;

	/**
	 * @brief Mark that a catchup just ended (orchestrator → COOLDOWN/NORMAL).
	 * Used to detect recurring catchup cycles caused by a large server backlog
	 * that tempo-based catchup cannot drain in one pass. The next time catchup
	 * would trigger within RECENT_CATCHUP_WINDOW_MS, skip-to-live fires instead
	 * so the demuxer reconnects and discards the buffered backlog.
	 */
	void mark_catchup_exit();

	// Public thresholds — accessible from worker thread (burst-skip logic).
	// Two-tier recovery strategy:
	//   drift ≤ INSTANT_JUMP_THRESHOLD_MS → SoundTouch tempo (≤3.0x), smooth audio
	//   drift >  INSTANT_JUMP_THRESHOLD_MS → burst-skip (packet discard, <1s, brief audio gap)
	static constexpr int64_t INSTANT_JUMP_THRESHOLD_MS = 2000; // 2s: above this use burst-skip
	static constexpr int64_t MAX_DRIFT_FOR_CATCHUP_MS = 12000; // 12s: above this skip-to-live

private:
	BufferState state_;

	static constexpr int64_t MIN_DRIFT_FOR_CATCHUP_MS = 800;    // SoundTouch trigger
	static constexpr int MIN_BUFFER_FRAMES = 0;
	static constexpr int64_t NETWORK_STABLE_THRESHOLD_MS = 200;
	static constexpr double TARGET_RECOVERY_SEC = 1.0;          // aggressive: drain drift in ~1 wall second
	static constexpr double MIN_CATCHUP_TEMPO = 1.05;
	static constexpr double MAX_CATCHUP_TEMPO = 3.0;            // 3.0x: live-stream priority over audio quality
	static constexpr int64_t RECENT_CATCHUP_WINDOW_MS = 8000;   // re-trigger within 8s → skip-to-live
};

} // namespace lss
