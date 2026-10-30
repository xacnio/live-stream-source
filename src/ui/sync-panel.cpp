// sync-panel.cpp
#include "ui/sync-panel.h"
#include "core/common.h"
#include "playout/sync-registry.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QCheckBox>
#include <QApplication>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>

namespace lss {

static constexpr const char *DOCK_ID = "lss-stream-sync";
static constexpr const char *SAVE_KEY = "lss_stream_sync";
// Settings a source held before the dock existed.
static constexpr const char *LEGACY_MAIN = "sync_source_uuid";
static constexpr const char *LEGACY_OFFSET = "sync_offset_ms";
static constexpr const char *LEGACY_HOLD = "sync_hold_ms";
static constexpr int REFRESH_MS = 500;

static bool g_dock_added = false;
static bool g_migrate_pending = false;

static QString text(const char *key) {
  return QString::fromUtf8(obs_module_text(key));
}

static bool is_live_source(obs_source_t *source) {
  const char *id = obs_source_get_unversioned_id(source);
  return id && std::strcmp(id, SOURCE_ID) == 0;
}

static std::vector<std::pair<std::string, std::string>> list_sources() {
  std::vector<std::pair<std::string, std::string>> found;
  obs_enum_sources(
      [](void *param, obs_source_t *source) {
        if (is_live_source(source)) {
          auto *list = static_cast<std::vector<std::pair<std::string, std::string>> *>(param);
          list->emplace_back(obs_source_get_name(source), obs_source_get_uuid(source));
        }
        return true;
      },
      &found);
  std::sort(found.begin(), found.end());
  return found;
}

SyncPanel::SyncPanel(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);

  auto *form = new QFormLayout();
  mainCombo = new QComboBox();
  form->addRow(text("SyncPanelMain"), mainCombo);

  modeCombo = new QComboBox();
  modeCombo->addItem(text("SyncModeOff"), static_cast<int>(SyncMode::Off));
  modeCombo->addItem(text("SyncModeFollow"), static_cast<int>(SyncMode::Follow));
  modeCombo->addItem(text("SyncModeFixed"), static_cast<int>(SyncMode::Fixed));
  form->addRow(text("SyncPanelMode"), modeCombo);
  layout->addLayout(form);

  modeHint = new QLabel();
  modeHint->setWordWrap(true);
  layout->addWidget(modeHint);

  followers = new QTableWidget(0, 4);
  followers->setHorizontalHeaderLabels({text("SyncPanelFollow"), text("SyncPanelSource"),
                                        text("SyncPanelOffset"), text("SyncPanelStatus")});
  followers->verticalHeader()->hide();
  followers->setSelectionMode(QAbstractItemView::NoSelection);
  followers->setEditTriggers(QAbstractItemView::NoEditTriggers);
  followers->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  followers->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  followers->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  followers->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
  layout->addWidget(followers, 1);

  auto *delayForm = new QFormLayout();
  mainDelay = new QSpinBox();
  mainDelay->setRange(-MAX_SYNC_OFFSET_MS, MAX_SYNC_OFFSET_MS);
  mainDelay->setSingleStep(10);
  mainDelay->setSuffix(" ms");
  mainDelay->setKeyboardTracking(false); // typing 500 mustn't move the picture for 5 and 50
  mainDelay->setToolTip(text("SyncPanelMainDelay.Desc"));
  delayForm->addRow(text("SyncPanelMainDelay"), mainDelay);
  layout->addLayout(delayForm);

  mayDelayMain = new QCheckBox(text("SyncPanelMayDelayMain"));
  layout->addWidget(mayDelayMain);

  matchButton = new QPushButton(text("SyncPanelMatch"));
  matchButton->setToolTip(text("SyncPanelMatch.Desc"));
  layout->addWidget(matchButton);

  statusLabel = new QLabel();
  statusLabel->setWordWrap(true);
  layout->addWidget(statusLabel);

  connect(mainCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int) { applyFromControls(); });
  connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int) { applyFromControls(); });
  connect(mainDelay, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [this](int) { applyFromControls(); });
  connect(mayDelayMain, &QCheckBox::toggled, this, [this](bool) { applyFromControls(); });
  connect(matchButton, &QPushButton::clicked, this, []() { SyncRegistry::instance().start_sync(); });

  timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, &SyncPanel::refresh);
  timer->start(REFRESH_MS);
  rebuild();
}

void SyncPanel::refresh() {
  SyncRegistry &registry = SyncRegistry::instance();
  const uint64_t revision = registry.revision();

  // A sync changes offsets from the playback thread, so saving hangs off this.
  if (revision == shown_revision_ && revision != saved_revision_) {
    saved_revision_ = revision;
    obs_frontend_save();
  }

  if (revision != shown_revision_ || list_sources() != sources_) {
    if (!followers->isAncestorOf(QApplication::focusWidget()) && !mainDelay->hasFocus())
      rebuild();
  }
  updateStatus();
}

void SyncPanel::rebuild() {
  building_ = true;
  SyncRegistry &registry = SyncRegistry::instance();
  const SyncConfig config = registry.config();
  shown_revision_ = registry.revision();
  sources_ = list_sources();

  mainCombo->clear();
  mainCombo->addItem(text("SyncPanelNoMain"), QString());
  for (const auto &source : sources_)
    mainCombo->addItem(QString::fromStdString(source.first), QString::fromStdString(source.second));
  const int index = mainCombo->findData(QString::fromStdString(config.main_uuid));
  mainCombo->setCurrentIndex(std::max(index, 0));

  modeCombo->setCurrentIndex(modeCombo->findData(static_cast<int>(config.mode)));
  mainDelay->setValue(config.main_delay_ms);
  mayDelayMain->setChecked(config.may_delay_main);

  std::vector<std::pair<std::string, std::string>> rows;
  for (const auto &source : sources_) {
    if (source.second != config.main_uuid)
      rows.push_back(source);
  }

  followers->setRowCount(static_cast<int>(rows.size()));
  for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
    const std::string uuid = rows[row].second;
    const auto entry = config.offsets_ms.find(uuid);
    const bool following = entry != config.offsets_ms.end();

    auto *follow = new QCheckBox();
    follow->setChecked(following);
    auto *holder = new QWidget();
    auto *holderLayout = new QHBoxLayout(holder);
    holderLayout->setContentsMargins(0, 0, 0, 0);
    holderLayout->setAlignment(Qt::AlignCenter);
    holderLayout->addWidget(follow);
    followers->setCellWidget(row, 0, holder);

    followers->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(rows[row].first)));

    auto *offset = new QSpinBox();
    offset->setRange(-MAX_SYNC_OFFSET_MS, MAX_SYNC_OFFSET_MS);
    offset->setSingleStep(10);
    offset->setSuffix(" ms");
    offset->setKeyboardTracking(false);
    offset->setValue(following ? entry->second : 0);
    offset->setEnabled(following);
    followers->setCellWidget(row, 2, offset);

    auto *status = new QTableWidgetItem();
    status->setData(Qt::UserRole, QString::fromStdString(uuid));
    followers->setItem(row, 3, status);

    connect(follow, &QCheckBox::toggled, this, [uuid, offset](bool on) {
      SyncRegistry &sync = SyncRegistry::instance();
      SyncConfig next = sync.config();
      if (on)
        next.add_follower(uuid, offset->value());
      else
        next.offsets_ms.erase(uuid);
      offset->setEnabled(on);
      sync.set_config(next);
    });
    connect(offset, QOverload<int>::of(&QSpinBox::valueChanged), this, [uuid](int value) {
      SyncRegistry::instance().set_offset(uuid, value);
    });
  }

  const SyncMode mode = static_cast<SyncMode>(modeCombo->currentData().toInt());
  modeHint->setText(text(mode == SyncMode::Follow  ? "SyncModeFollow.Desc"
                         : mode == SyncMode::Fixed ? "SyncModeFixed.Desc"
                                                   : "SyncModeOff.Desc"));
  building_ = false;
}

void SyncPanel::applyFromControls() {
  if (building_)
    return;

  SyncRegistry &registry = SyncRegistry::instance();
  SyncConfig next = registry.config();
  next.set_main(mainCombo->currentData().toString().toStdString());
  next.mode = static_cast<SyncMode>(modeCombo->currentData().toInt());
  next.main_delay_ms = mainDelay->value();
  next.may_delay_main = mayDelayMain->isChecked();
  registry.set_config(next);
  rebuild();
}

static QString progress_text(const SyncProgress &progress) {
  switch (progress.state) {
  case SyncListenState::Listening:
    return text("SyncStatusListening")
        .arg(progress.seconds_left)
        .arg(progress.heard_main)
        .arg(progress.heard_own);
  case SyncListenState::Found:
    if (progress.main_delay_ms > 0)
      return text("SyncStatusMatchedHeld").arg(progress.delta_ms).arg(progress.main_delay_ms);
    if (progress.short_ms > 0)
      return text("SyncStatusMatchedShort").arg(progress.short_ms);
    return text("SyncStatusMatched").arg(progress.delta_ms);
  case SyncListenState::NotFound:
    return text("SyncStatusNotFound").arg(progress.heard_main).arg(progress.heard_own);
  case SyncListenState::NoSound:
    return text(progress.heard_main == 0 ? "SyncStatusNoSoundMain" : "SyncStatusNoSoundOwn");
  case SyncListenState::NoStream:
    return text("SyncStatusNotStarted");
  default:
    return QString();
  }
}

void SyncPanel::updateStatus() {
  SyncRegistry &registry = SyncRegistry::instance();
  const SyncConfig config = registry.config();
  const int64_t now = static_cast<int64_t>(os_gettime_ns() / 1000);
  bool listening = false;

  for (int row = 0; row < followers->rowCount(); ++row) {
    QTableWidgetItem *item = followers->item(row, 3);
    if (!item)
      continue;
    const std::string uuid = item->data(Qt::UserRole).toString().toStdString();

    QString status;
    SyncState state;
    const SyncProgress progress = registry.progress(uuid, now);
    if (progress.state == SyncListenState::Listening)
      listening = true;

    if (!config.is_follower(uuid)) {
      status = text("SyncStatusNotInGroup");
    } else if (progress.state != SyncListenState::Idle) {
      status = progress_text(progress);
    } else if (!registry.lookup(uuid, state) || now - state.updated_us > SYNC_PANEL_STALE_US) {
      status = text("SyncStatusNoStream");
    } else if (config.mode == SyncMode::Follow) {
      status = state.following ? text("SyncStatusFollowing").arg(state.error_ms)
                               : text("SyncStatusWaiting");
    } else {
      status = text("SyncStatusFixed");
    }
    item->setText(status);
  }

  matchButton->setEnabled(config.ready() && !listening);
  matchButton->setText(text(listening ? "SyncPanelListening" : "SyncPanelMatch"));
  statusLabel->setText(config.ready() ? (listening ? text("SyncPanelClapNow") : QString())
                                      : text("SyncPanelNotReady"));
}

// Kept with the scene collection, because source uuids only mean something there.
static void save_config(obs_data_t *save_data) {
  const SyncConfig config = SyncRegistry::instance().config();

  obs_data_t *data = obs_data_create();
  obs_data_set_int(data, "mode", static_cast<int>(config.mode));
  obs_data_set_string(data, "main", config.main_uuid.c_str());
  obs_data_set_int(data, "main_delay_ms", config.main_delay_ms);
  obs_data_set_bool(data, "may_delay_main", config.may_delay_main);

  obs_data_array_t *list = obs_data_array_create();
  for (const auto &follower : config.offsets_ms) {
    obs_data_t *item = obs_data_create();
    obs_data_set_string(item, "uuid", follower.first.c_str());
    obs_data_set_int(item, "offset_ms", follower.second);
    obs_data_array_push_back(list, item);
    obs_data_release(item);
  }
  obs_data_set_array(data, "followers", list);
  obs_data_array_release(list);

  obs_data_set_obj(save_data, SAVE_KEY, data);
  obs_data_release(data);
}

static void load_config(obs_data_t *save_data) {
  obs_data_t *data = obs_data_get_obj(save_data, SAVE_KEY);
  if (!data) {
    SyncRegistry::instance().set_config(SyncConfig{});
    g_migrate_pending = true;
    return;
  }

  SyncConfig config;
  config.mode = static_cast<SyncMode>(obs_data_get_int(data, "mode"));
  config.main_uuid = obs_data_get_string(data, "main");
  config.main_delay_ms = static_cast<int>(obs_data_get_int(data, "main_delay_ms"));
  obs_data_set_default_bool(data, "may_delay_main", true);
  config.may_delay_main = obs_data_get_bool(data, "may_delay_main");

  obs_data_array_t *list = obs_data_get_array(data, "followers");
  const size_t count = obs_data_array_count(list);
  for (size_t i = 0; i < count; ++i) {
    obs_data_t *item = obs_data_array_item(list, i);
    config.add_follower(obs_data_get_string(item, "uuid"),
                        static_cast<int>(obs_data_get_int(item, "offset_ms")));
    obs_data_release(item);
  }
  obs_data_array_release(list);
  obs_data_release(data);

  SyncRegistry::instance().set_config(config);
}

// Each source used to name its own main source. The first one found decides, and
// a source that was itself being followed can't also follow.
static void migrate_legacy_settings() {
  SyncConfig config;
  obs_enum_sources(
      [](void *param, obs_source_t *source) {
        if (!is_live_source(source))
          return true;
        auto *next = static_cast<SyncConfig *>(param);
        obs_data_t *settings = obs_source_get_settings(source);
        const std::string main_uuid = obs_data_get_string(settings, LEGACY_MAIN);
        const int offset = static_cast<int>(obs_data_get_int(settings, LEGACY_OFFSET));
        const std::string self = obs_source_get_uuid(source);
        if (!main_uuid.empty()) {
          if (next->main_uuid.empty()) {
            next->main_uuid = main_uuid;
            next->mode = SyncMode::Follow;
          }
          next->add_follower(self, offset);
        }
        obs_data_release(settings);
        return true;
      },
      &config);

  obs_enum_sources(
      [](void *param, obs_source_t *source) {
        if (!is_live_source(source))
          return true;
        auto *next = static_cast<SyncConfig *>(param);
        obs_data_t *settings = obs_source_get_settings(source);
        if (next->main_uuid == obs_source_get_uuid(source))
          next->main_delay_ms = static_cast<int>(obs_data_get_int(settings, LEGACY_HOLD));
        obs_data_erase(settings, LEGACY_MAIN);
        obs_data_erase(settings, LEGACY_OFFSET);
        obs_data_erase(settings, LEGACY_HOLD);
        obs_data_release(settings);
        return true;
      },
      &config);

  if (config.ready()) {
    lss_log_info("Sync: moved the delay match settings into the Stream Sync dock");
    SyncRegistry::instance().set_config(config);
  }
}

static void on_save(obs_data_t *save_data, bool saving, void *) {
  if (saving)
    save_config(save_data);
  else
    load_config(save_data);
}

static void on_frontend_event(enum obs_frontend_event event, void *) {
  switch (event) {
  case OBS_FRONTEND_EVENT_FINISHED_LOADING:
    // Not at module load: the main window doesn't exist yet, and without a UI
    // there's nothing to build this on.
    if (!g_dock_added) {
      g_dock_added = true;
      auto *window = static_cast<QWidget *>(obs_frontend_get_main_window());
      obs_frontend_add_dock_by_id(DOCK_ID, obs_module_text("SyncPanelTitle"), new SyncPanel(window));
    }
    [[fallthrough]];
  case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
    if (g_migrate_pending) {
      g_migrate_pending = false;
      migrate_legacy_settings();
    }
    break;
  case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
    SyncRegistry::instance().set_config(SyncConfig{});
    break;
  default:
    break;
  }
}

void init_sync_panel() {
  obs_frontend_add_save_callback(on_save, nullptr);
  obs_frontend_add_event_callback(on_frontend_event, nullptr);
}

void shutdown_sync_panel() {
  obs_frontend_remove_event_callback(on_frontend_event, nullptr);
  obs_frontend_remove_save_callback(on_save, nullptr);
}

} // namespace lss
