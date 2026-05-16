// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025

#pragma once

#include <cstdint>

namespace lss {

// Forward declarations
class BufferManager;
class TempoRamper;
class AudioTimeStretcher;

/**
 * @brief Catch-up state machine states
 * 
 * State transitions:
 * NORMAL → STARTING → ACTIVE → EXITING → COOLDOWN → NORMAL
 */
enum class CatchupState {
	NORMAL,    // Normal playback (1.0x tempo)
	STARTING,  // Ramping tempo up to target
	ACTIVE,    // Playing at elevated tempo
	EXITING,   // Ramping tempo down to 1.0x
	COOLDOWN   // Post-catch-up cooldown period
};

/**
 * @brief Catch-up orchestrator - coordinates all catch-up components
 * 
 * This is the main coordinator for the catch-up system.
 * It implements a robust state machine that manages tempo transitions,
 * prevents catch-up loops, and ensures smooth A/V sync.
 * 
 * State Machine:
 * 
 * NORMAL:
 *   - Normal playback at 1.0x tempo
 *   - Monitors buffer for catch-up conditions
 *   - Transition: buffer.should_start_catchup() && !in_cooldown → STARTING
 * 
 * STARTING:
 *   - Ramping tempo up to target
 *   - Target tempo calculated by BufferManager
 *   - Transition: ramper.is_stable() → ACTIVE
 * 
 * ACTIVE:
 *   - Playing at elevated tempo (1.05x - 1.6x)
 *   - Tracking deficit reduction
 *   - Transition: deficit < 280ms → EXITING
 *
 * EXITING:
 *   - Ramping tempo down to 1.0x
 *   - Smooth transition back to normal
 *   - Transition: ramper.is_stable() → COOLDOWN
 *
 * COOLDOWN:
 *   - 3-second cooldown period
 *   - Prevents immediate re-entry to catch-up
 *   - Transition: cooldown_expired → NORMAL
 * 
 * Usage:
 *   CatchupOrchestrator orchestrator(sync, buffer, ramper, stretcher);
 *   
 *   // Every frame:
 *   orchestrator.update(os_gettime_ns());
 *   double tempo = orchestrator.get_tempo();
 *   
 *   // Use tempo for audio stretching and clock advancement
 */
class CatchupOrchestrator {
public:
	CatchupOrchestrator(BufferManager &buffer, TempoRamper &ramper,
			    AudioTimeStretcher &stretcher);

	/**
	 * @brief Update state machine (call every frame)
	 * @param wall_clock_ns Current wall clock time in nanoseconds
	 * 
	 * This is the main entry point. Call this once per video frame
	 * to update the catch-up state machine.
	 */
	void update(int64_t wall_clock_ns);

	/**
	 * @brief Get current state
	 * @return Current catch-up state
	 */
	CatchupState get_state() const;

	/**
	 * @brief Get current tempo
	 * @return Current tempo (1.0 = normal, 2.0 = double speed)
	 * 
	 * This is the tempo that should be used for audio stretching
	 * and clock advancement.
	 */
	double get_tempo() const;

	/**
	 * @brief Check if catch-up is active
	 * @return true if in STARTING, ACTIVE, or EXITING state
	 */
	bool is_active() const;

	/**
	 * @brief Force exit from catch-up (for manual skip)
	 * 
	 * Immediately transitions to EXITING state, regardless of
	 * current state. Used when user manually skips to live.
	 */
	void force_exit();

private:
	/**
	 * @brief Transition to a new state
	 * @param new_state Target state
	 * 
	 * Handles state transition logging and cleanup.
	 */
	void transition_to(CatchupState new_state);

	/**
	 * @brief Update logic for NORMAL state
	 * 
	 * Monitors buffer for catch-up conditions.
	 * Transitions to STARTING if conditions are met.
	 */
	void update_normal();

	/**
	 * @brief Update logic for STARTING state
	 * 
	 * Waits for tempo to ramp up to target.
	 * Transitions to ACTIVE when stable.
	 */
	void update_starting();

	/**
	 * @brief Update logic for ACTIVE state
	 * 
	 * Monitors deficit reduction.
	 * Transitions to EXITING when caught up.
	 */
	void update_active();

	/**
	 * @brief Update logic for EXITING state
	 * 
	 * Waits for tempo to ramp down to 1.0x.
	 * Transitions to COOLDOWN when stable.
	 */
	void update_exiting();

	/**
	 * @brief Update logic for COOLDOWN state
	 * 
	 * Waits for cooldown period to expire.
	 * Transitions to NORMAL when expired.
	 */
	void update_cooldown();

	CatchupState state_;
	BufferManager &buffer_;
	TempoRamper &ramper_;
	AudioTimeStretcher &stretcher_;

	int64_t cooldown_until_ns_;
	int64_t deficit_ns_;
	int64_t initial_deficit_ns_;  // deficit at catchup start, for time-based exit
	int64_t active_start_ns_;     // wall time when ACTIVE state began
	int burst_quiet_frames_;      // consecutive frames with quiet network → burst ending

	// Cooldown duration: 3 seconds — prevents re-trigger from residual burst frames.
	static constexpr int64_t COOLDOWN_DURATION_NS = 3000LL * 1000000LL;

	// Exit threshold: 280ms — above the 200ms headroom baseline so exit is stable.
	static constexpr int64_t EXIT_DEFICIT_THRESHOLD_NS = 280LL * 1000000LL;
};

} // namespace lss
