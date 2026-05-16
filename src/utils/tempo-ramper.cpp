// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025

#include "tempo-ramper.h"
#include "core/common.h"
#include <cmath>

namespace lss {

// Constructor - Initialize to 1.0x tempo
TempoRamper::TempoRamper()
	: current_(1.0), target_(1.0), ramp_speed_(DEFAULT_RAMP_SPEED)
{
	lss_log_debug("TempoRamper created: current=1.0x, target=1.0x");
}

void TempoRamper::set_ramp_speed(double speed)
{
	if (speed > 0.0 && speed <= 1.0) {
		ramp_speed_ = speed;
	}
}

// Set target tempo
void TempoRamper::set_target(double target_tempo)
{
	if (target_tempo != target_) {
		double old_target = target_;
		target_ = target_tempo;
		lss_log_debug("TempoRamper::set_target: %.4fx → %.4fx (current=%.4fx)",
		              old_target, target_, current_);
	}
}

// Get current tempo (smoothly ramped)
double TempoRamper::get_current()
{
	// Calculate difference between current and target
	double diff = target_ - current_;
	
	// Check if we're close enough to target (within 1%)
	if (std::abs(diff) > STABILITY_THRESHOLD) {
		// Ramp toward target by configured fraction of the difference
		double old_current = current_;
		current_ += diff * ramp_speed_;
		
		lss_log_debug("TempoRamper::get_current: %.4fx → %.4fx (target=%.4fx, diff=%.4f)",
		              old_current, current_, target_, diff);
	} else {
		// Close enough - snap to target
		if (current_ != target_) {
			lss_log_debug("TempoRamper::get_current: %.4fx → %.4fx (STABLE)",
			              current_, target_);
			current_ = target_;
		}
	}
	
	return current_;
}

// Check if tempo has reached target
bool TempoRamper::is_stable() const
{
	return std::abs(target_ - current_) <= STABILITY_THRESHOLD;
}

// Reset to 1.0x tempo
void TempoRamper::reset()
{
	double old_current = current_;
	double old_target = target_;
	
	current_ = 1.0;
	target_ = 1.0;
	ramp_speed_ = DEFAULT_RAMP_SPEED;

	lss_log_debug("TempoRamper::reset: current %.4fx → 1.0x, target %.4fx → 1.0x",
	              old_current, old_target);
}

} // namespace lss
