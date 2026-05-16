// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025

#pragma once

namespace lss {

/**
 * @brief Smooth tempo transition controller
 * 
 * Gradually ramps tempo changes to prevent audible artifacts.
 * Uses exponential smoothing to transition between tempo values.
 * 
 * Ramping Logic:
 * - Ramp speed: 30% per frame (configurable)
 * - Stability threshold: 1% (tempo within 1% of target)
 * - No instant tempo changes - always smooth
 * 
 * Example:
 *   TempoRamper ramper;
 *   ramper.set_target(1.5);
 *   
 *   // Over several frames:
 *   double tempo1 = ramper.get_current(); // 1.15 (30% toward 1.5)
 *   double tempo2 = ramper.get_current(); // 1.255 (30% more)
 *   double tempo3 = ramper.get_current(); // 1.3285 (30% more)
 *   // ... continues until stable at 1.5
 */
class TempoRamper {
public:
	TempoRamper();

	/**
	 * @brief Set target tempo
	 * @param target_tempo Target tempo to ramp toward (0.5 - 4.0)
	 *
	 * The ramper will gradually transition from current tempo
	 * to the target tempo over multiple frames.
	 */
	void set_target(double target_tempo);

	/**
	 * @brief Override the per-call ramp speed (default 0.3 = 30% toward target).
	 * Use a smaller value (e.g. 0.05) for a smoother audible transition when
	 * dropping back to 1.0x — SoundTouch produces clicks if tempo changes too
	 * abruptly across its ~40ms sequence window.
	 */
	void set_ramp_speed(double speed);

	/**
	 * @brief Get current tempo (smoothly ramped)
	 * @return Current tempo value
	 * 
	 * Call this every frame to get the smoothly ramped tempo.
	 * The tempo will gradually approach the target value.
	 */
	double get_current();

	/**
	 * @brief Check if tempo has reached target
	 * @return true if tempo is within 1% of target
	 * 
	 * Use this to detect when the tempo transition is complete.
	 */
	bool is_stable() const;

	/**
	 * @brief Reset to 1.0x tempo
	 * 
	 * Immediately resets both current and target tempo to 1.0x.
	 * No ramping occurs.
	 */
	void reset();

private:
	double current_;
	double target_;
	double ramp_speed_;

	// Default ramp speed: 30% per call (fast — good for ramp-up).
	static constexpr double DEFAULT_RAMP_SPEED = 0.3;

	// Stability threshold: 1%
	static constexpr double STABILITY_THRESHOLD = 0.01;
};

} // namespace lss
