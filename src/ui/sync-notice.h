// sync-notice.h
#pragma once

#include <string>

namespace lss {

// Shows text in the OBS status bar and the log. Safe from any thread. A status
// bar message doesn't take focus, which matters while someone is on air.
void show_status_notice(const std::string &text);

} // namespace lss
