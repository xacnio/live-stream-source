// context-buttons.h - refresh and skip to live in OBS's bar under the preview
//
// OBS has no API for this bar, so the buttons go into its layout by object name,
// beside Properties and Filters, the part it doesn't rebuild. If a future OBS
// renames those widgets the buttons simply don't appear.
#pragma once

#include <obs.h>

#include <QWidget>

class QPushButton;
class QTimer;

namespace lss {

class ContextButtons : public QWidget {
public:
  // Null if the window doesn't have the expected layout.
  static ContextButtons *attach(QWidget *window);

  ~ContextButtons() override;

  void stop();

private:
  ContextButtons(QWidget *properties_button, QWidget *parent);

  void refresh();
  void press(const char *button);

  QWidget *properties_button_ = nullptr;
  QPushButton *refresh_ = nullptr;
  QPushButton *skip_ = nullptr;
  QTimer *timer_ = nullptr;
  obs_weak_source_t *target_ = nullptr;
};

void init_context_buttons();
void shutdown_context_buttons();

} // namespace lss
