// fallback-dialog.h - a window of its own for the fallback settings
//
// OBS's properties window stacks every field in one narrow column, which three
// fallbacks with twenty fields each don't fit in.
#pragma once

#include <obs.h>

#include <QDialog>

#include <functional>
#include <string>
#include <vector>

namespace lss {

class FallbackPage;

using FallbackApply = std::function<void(obs_data_t *changes)>;

class FallbackDialog : public QDialog {
public:
  // scene_sources are the names offered for the "source in the scene" option.
  FallbackDialog(obs_data_t *settings, const std::vector<std::string> &scene_sources,
                 FallbackApply apply, QWidget *parent = nullptr);

  void apply_changes();

private:
  std::vector<FallbackPage *> pages_;
  FallbackApply apply_;
};

// UI thread. Returns once the window is closed.
void open_fallback_dialog(obs_data_t *settings, const std::vector<std::string> &scene_sources,
                          FallbackApply apply);

} // namespace lss
