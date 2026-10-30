// sync-panel.h - the dock where sources are grouped for delay sync
#pragma once

#include <QWidget>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace lss {

// One main source and any number of followers, kept with the scene collection.
// Having the choice in one place means two sources can't point at each other.
class SyncPanel : public QWidget {
  Q_OBJECT

public:
  explicit SyncPanel(QWidget *parent = nullptr);

private:
  void refresh();
  void rebuild();
  void updateStatus();
  void applyFromControls();

  QComboBox *mainCombo = nullptr;
  QComboBox *modeCombo = nullptr;
  QLabel *modeHint = nullptr;
  QTableWidget *followers = nullptr;
  QSpinBox *mainDelay = nullptr;
  QCheckBox *mayDelayMain = nullptr;
  QPushButton *matchButton = nullptr;
  QLabel *statusLabel = nullptr;
  QTimer *timer = nullptr;

  std::vector<std::pair<std::string, std::string>> sources_; // name, uuid
  uint64_t shown_revision_ = 0;
  uint64_t saved_revision_ = 0;
  bool building_ = false;
};

void init_sync_panel();
void shutdown_sync_panel();

} // namespace lss
