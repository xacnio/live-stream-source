// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025

#include "catchup-orchestrator.h"
#include "common.h"
#include "utils/buffer-manager.h"
#include "utils/tempo-ramper.h"
#include "media/audio-time-stretcher.h"

namespace lss {

// Helper function to convert state enum to string
static const char *state_to_string(CatchupState state)
{
	switch (state) {
	case CatchupState::NORMAL:
		return "NORMAL";
	case CatchupState::STARTING:
		return "STARTING";
	case CatchupState::ACTIVE:
		return "ACTIVE";
	case CatchupState::EXITING:
		return "EXITING";
	case CatchupState::COOLDOWN:
		return "COOLDOWN";
	default:
		return "UNKNOWN";
	}
}

// Constructor - inject dependencies
CatchupOrchestrator::CatchupOrchestrator(BufferManager &buffer,
					 TempoRamper &ramper,
					 AudioTimeStretcher &stretcher)
	: state_(CatchupState::NORMAL),
	  buffer_(buffer),
	  ramper_(ramper),
	  stretcher_(stretcher),
	  cooldown_until_ns_(0),
	  deficit_ns_(0),
	  initial_deficit_ns_(0),
	  active_start_ns_(0),
	  burst_quiet_frames_(0)
{
	lss_log_info("CatchupOrchestrator created");
}

// Update state machine (call every frame)
void CatchupOrchestrator::update(int64_t wall_clock_ns)
{
	switch (state_) {
	case CatchupState::NORMAL:
		update_normal();
		break;
	case CatchupState::STARTING:
		update_starting();
		break;
	case CatchupState::ACTIVE:
		update_active();
		break;
	case CatchupState::EXITING:
		update_exiting();
		break;
	case CatchupState::COOLDOWN:
		update_cooldown();
		break;
	}
}

// Get current state
CatchupState CatchupOrchestrator::get_state() const
{
	return state_;
}

// Get current tempo
double CatchupOrchestrator::get_tempo() const
{
	return ramper_.get_current();
}

// Check if catch-up is active
bool CatchupOrchestrator::is_active() const
{
	return state_ == CatchupState::STARTING ||
	       state_ == CatchupState::ACTIVE ||
	       state_ == CatchupState::EXITING;
}

// Force exit from catch-up (for manual skip)
void CatchupOrchestrator::force_exit()
{
	if (is_active()) {
		lss_log_info("CatchupOrchestrator: Force exit requested");
		transition_to(CatchupState::EXITING);
	}
}

// Transition to a new state
void CatchupOrchestrator::transition_to(CatchupState new_state)
{
	if (state_ == new_state) {
		return;
	}

	if (new_state == CatchupState::ACTIVE) {
		active_start_ns_ = os_gettime_ns();
		// Re-sample the current headroom as the effective drain target.
		// By ACTIVE entry, the STARTING ramp + pacing has already consumed
		// part of the initial burst, so the original trigger-time headroom
		// would over-run the live edge and trigger a burst dump in COOLDOWN.
		initial_deficit_ns_ = buffer_.get_state().drift_from_live_ms * 1000000LL;
		burst_quiet_frames_ = 0;
	}

	if (new_state == CatchupState::EXITING) {
		// Instant drop to 1.0x — counterintuitive but cleaner than ramping.
		// Each tempo step the ramper produced was a separate SoundTouch
		// setTempo() call, and SoundTouch can't smoothly re-blend its
		// internal sequence buffer across many micro-changes (~30+ steps
		// over 300ms). The result was a string of tiny clicks ("pıt pıt").
		// One single setTempo(1.0) call lets the audio decoder's existing
		// stretcher_.reset() flush the buffer once, cleanly, instead.
		ramper_.set_ramp_speed(1.0);
		ramper_.set_target(1.0);
	}

	if (new_state == CatchupState::NORMAL) {
		// Safety reset — guarantees we never leave NORMAL with the audio
		// stretcher still at catchup tempo. (The 1.267x-stuck bug from logs.)
		ramper_.reset();
		if (state_ == CatchupState::COOLDOWN) {
			buffer_.mark_catchup_exit();
		}
	}

	const char *old_state_name = state_to_string(state_);
	const char *new_state_name = state_to_string(new_state);

	lss_log_info("CatchupOrchestrator: State transition: %s → %s "
	             "(tempo=%.4fx, deficit=%lld ns)",
	             old_state_name, new_state_name, get_tempo(), deficit_ns_);

	state_ = new_state;
}

// Update logic for NORMAL state
void CatchupOrchestrator::update_normal()
{
	// Check if we should start catch-up
	if (buffer_.should_start_catchup()) {
		// Check if we're still in cooldown
		int64_t current_time_ns = os_gettime_ns();
		if (current_time_ns < cooldown_until_ns_) {
			int64_t remaining_ms =
				(cooldown_until_ns_ - current_time_ns) / 1000000LL;
			lss_log_debug("CatchupOrchestrator: Catch-up blocked by cooldown "
			              "(remaining=%lld ms)",
			              remaining_ms);
			return;
		}

		// Calculate target tempo
		double target_tempo = buffer_.calculate_optimal_tempo();

		// Set target tempo for ramper
		ramper_.set_target(target_tempo);

		// Initialize deficit tracking
		deficit_ns_ = buffer_.get_state().drift_from_live_ms * 1000000LL;
		initial_deficit_ns_ = deficit_ns_;

		lss_log_info("CatchupOrchestrator: Starting catch-up "
		             "(target_tempo=%.4fx, initial_deficit=%lld ns)",
		             target_tempo, deficit_ns_);

		// Transition to STARTING
		transition_to(CatchupState::STARTING);
	}
}

// Update logic for STARTING state
void CatchupOrchestrator::update_starting()
{
	// Wait for tempo to ramp up to target
	if (ramper_.is_stable()) {
		lss_log_info("CatchupOrchestrator: Tempo stable at %.4fx, "
		             "entering ACTIVE state",
		             get_tempo());

		// Transition to ACTIVE
		transition_to(CatchupState::ACTIVE);
	} else {
		lss_log_debug("CatchupOrchestrator: Ramping tempo up "
		              "(current=%.4fx, stable=%s)",
		              get_tempo(), ramper_.is_stable() ? "yes" : "no");
	}
}

// Update logic for ACTIVE state
void CatchupOrchestrator::update_active()
{
	int64_t elapsed_ns = os_gettime_ns() - active_start_ns_;
	double gain_ns = static_cast<double>(elapsed_ns) * (get_tempo() - 1.0);
	deficit_ns_ = initial_deficit_ns_ - static_cast<int64_t>(gain_ns);
	if (deficit_ns_ < 0)
		deficit_ns_ = 0;

	// Detect end of TCP burst via inter-packet timing. During burst the
	// server delivers packets back-to-back (network_delay_ms < 10ms). Once
	// it returns to real-time pacing, gaps grow to ≥10ms. Require sustained
	// quiet network (5 consecutive frames ≈ 165ms) so a single late audio
	// packet doesn't fool us into exiting mid-burst — which is exactly what
	// caused the multiple back-to-back catchup cycles before this fix.
	int64_t cur_delay = buffer_.get_state().network_delay_ms;
	if (cur_delay >= 10) {
		burst_quiet_frames_++;
	} else {
		burst_quiet_frames_ = 0;
	}
	bool burst_over = burst_quiet_frames_ >= 3;

	bool deficit_drained = deficit_ns_ < EXIT_DEFICIT_THRESHOLD_NS;
	// Safety cap so a misread burst signal can't trap us in ACTIVE forever.
	bool safety_timeout = elapsed_ns > 30LL * 1000000000LL;

	// Primary exit: the actual stream caught up. When real drift drops below
	// the threshold, we've consumed enough buffer — staying at high tempo any
	// longer over-consumes (audio decoder pulls 3x faster than server delivers)
	// and the audio queue underruns, producing the click at exit. This must
	// fire before the safety re-anchor (drift < -800ms) which slams force_exit
	// and the abrupt tempo drop the user heard as "pıt pıt".
	int64_t real_drift_ms = buffer_.get_state().drift_from_live_ms;
	bool real_caught_up = real_drift_ms < (EXIT_DEFICIT_THRESHOLD_NS / 1000000LL);

	lss_log_debug("CatchupOrchestrator: ACTIVE state "
	              "(tempo=%.4fx, deficit=%lld ns, elapsed=%lld ms, "
	              "real_drift=%lld ms, net_delay=%lld ms, quiet=%d)",
	              get_tempo(), deficit_ns_, elapsed_ns / 1000000LL,
	              real_drift_ms, cur_delay, burst_quiet_frames_);

	if (real_caught_up || (deficit_drained && burst_over) || safety_timeout) {
		lss_log_info("CatchupOrchestrator: Caught up (real_drift=%lld ms, "
		             "deficit=%lld ns, elapsed=%lld ms, real=%d burst_over=%d "
		             "safety=%d), starting exit",
		             real_drift_ms, deficit_ns_, elapsed_ns / 1000000LL,
		             (int)real_caught_up, (int)burst_over, (int)safety_timeout);
		ramper_.set_target(1.0);
		transition_to(CatchupState::EXITING);
	}

	if (buffer_.should_skip_to_live()) {
		lss_log_warn("CatchupOrchestrator: Drift too large, should skip to live");
		force_exit();
	}
}

// Update logic for EXITING state
void CatchupOrchestrator::update_exiting()
{
	// Wait for tempo to ramp down to 1.0x
	if (ramper_.is_stable()) {
		lss_log_info("CatchupOrchestrator: Tempo stable at 1.0x, "
		             "entering COOLDOWN state");

		// Set cooldown timer
		cooldown_until_ns_ = os_gettime_ns() + COOLDOWN_DURATION_NS;

		// Transition to COOLDOWN
		transition_to(CatchupState::COOLDOWN);
	} else {
		lss_log_debug("CatchupOrchestrator: Ramping tempo down "
		              "(current=%.4fx, stable=%s)",
		              get_tempo(), ramper_.is_stable() ? "yes" : "no");
	}
}

// Update logic for COOLDOWN state
void CatchupOrchestrator::update_cooldown()
{
	// Check if cooldown period has expired
	int64_t current_time_ns = os_gettime_ns();
	if (current_time_ns >= cooldown_until_ns_) {
		lss_log_info("CatchupOrchestrator: Cooldown expired, "
		             "returning to NORMAL state");

		// Transition to NORMAL
		transition_to(CatchupState::NORMAL);
	} else {
		int64_t remaining_ms =
			(cooldown_until_ns_ - current_time_ns) / 1000000LL;
		lss_log_debug("CatchupOrchestrator: COOLDOWN state "
		              "(remaining=%lld ms)",
		              remaining_ms);
	}
}

} // namespace lss
