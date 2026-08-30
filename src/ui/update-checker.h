// update-checker.h - GitHub release update notification
#pragma once

namespace lss {

// Registers the startup check. Call from obs_module_load().
void init_update_checker();

// Aborts any in-flight check and joins its thread. Call from obs_module_unload().
void shutdown_update_checker();

// `silent` hides the up-to-date and failure dialogs.
void check_for_updates(bool silent);

bool update_check_on_startup_enabled();
void set_update_check_on_startup(bool enabled);

} // namespace lss
