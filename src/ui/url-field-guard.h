// url-field-guard.h - keeps a filled-in stream URL out of sight and out of reach
//
// OBS's password field hides the text but stays editable, and its button says
// Show/Hide. This makes it read-only until revealed and puts an eye on the button.
// OBS has no API for that, so the field is found by its tooltip. If that ever stops
// matching, the field is still hidden, just editable.
#pragma once

namespace lss {

// Any thread. Call once the properties are being built or rebuilt.
void protect_url_field();

} // namespace lss
