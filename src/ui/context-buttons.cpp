// context-buttons.cpp
#include "ui/context-buttons.h"
#include "core/common.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAbstractButton>
#include <QBoxLayout>
#include <QPointer>
#include <QPushButton>
#include <QTimer>

#include <cstring>

namespace lss {

static constexpr int REFRESH_MS = 250;

static QPointer<ContextButtons> g_buttons; // the bar owns it, and can go first

static QString text(const char *key) {
  return QString::fromUtf8(obs_module_text(key));
}

// The enumeration runs bottom to top, and OBS's bar follows the topmost selected item.
static bool find_selected(obs_scene_t *, obs_sceneitem_t *item, void *param) {
  if (obs_sceneitem_selected(item))
    *static_cast<obs_source_t **>(param) = obs_sceneitem_get_source(item);
  if (obs_sceneitem_is_group(item))
    obs_sceneitem_group_enum_items(item, find_selected, param);
  return true;
}

ContextButtons *ContextButtons::attach(QWidget *window) {
  auto *bar = window ? window->findChild<QWidget *>("contextSubContainer") : nullptr;
  auto *last = window ? window->findChild<QWidget *>("sourceInteractButton") : nullptr;
  auto *properties = window ? window->findChild<QWidget *>("sourcePropertiesButton") : nullptr;
  auto *layout = bar ? qobject_cast<QBoxLayout *>(bar->layout()) : nullptr;
  if (!last || !properties || !layout || layout->indexOf(last) < 0) {
    lss_log_warn("Context bar: OBS's layout isn't the expected one, leaving it alone");
    return nullptr;
  }

  auto *buttons = new ContextButtons(properties, bar);
  layout->insertWidget(layout->indexOf(last) + 1, buttons);
  return buttons;
}

ContextButtons::ContextButtons(QWidget *properties_button, QWidget *parent)
    : QWidget(parent), properties_button_(properties_button) {
  auto *row = new QHBoxLayout(this);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(6);

  const auto add_button = [&](const char *label, const char *icon, const char *property) {
    auto *button = new QPushButton();
    button->setMinimumHeight(22);
    button->setToolTip(text(label));
    button->setProperty("class", icon); // the theme picks the icon
    row->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, property] { press(property); });
    return button;
  };
  refresh_ = add_button("RefreshBtn", "icon-refresh", PROP_REFRESH);
  skip_ = add_button("SkipToLiveBtn", "icon-media-next", PROP_SKIP_TO_LIVE);

  hide();
  timer_ = new QTimer(this);
  connect(timer_, &QTimer::timeout, this, &ContextButtons::refresh);
  timer_->start(REFRESH_MS);
}

ContextButtons::~ContextButtons() {
  stop();
}

void ContextButtons::stop() {
  timer_->stop();
  obs_weak_source_release(target_);
  target_ = nullptr;
}

// OBS doesn't say what's selected, and there's no signal that survives a change
// of scene or scene collection, so this looks.
void ContextButtons::refresh() {
  if (!parentWidget()->isVisible())
    return;

  obs_source_t *scene_source = obs_frontend_preview_program_mode_active()
                                   ? obs_frontend_get_current_preview_scene()
                                   : obs_frontend_get_current_scene();
  obs_source_t *selected = nullptr;
  if (obs_scene_t *scene = obs_scene_from_source(scene_source))
    obs_scene_enum_items(scene, find_selected, &selected);

  const bool ours = selected && std::strcmp(obs_source_get_unversioned_id(selected), SOURCE_ID) == 0;
  obs_weak_source_release(target_);
  target_ = ours ? obs_source_get_weak_source(selected) : nullptr;
  obs_source_release(scene_source);

  setVisible(ours);

  // OBS drops the text from its own buttons when the window gets narrow.
  const auto *reference = qobject_cast<QAbstractButton *>(properties_button_);
  const bool compact = reference && reference->text().isEmpty();
  refresh_->setText(compact ? QString() : text("RefreshBtn"));
  skip_->setText(compact ? QString() : text("SkipToLiveBtn"));
}

// The same buttons as in the properties window, so there's one way to do either.
void ContextButtons::press(const char *button) {
  obs_source_t *source = obs_weak_source_get_source(target_);
  if (!source)
    return;

  obs_properties_t *props = obs_source_properties(source);
  if (obs_property_t *property = obs_properties_get(props, button))
    obs_property_button_clicked(property, source);
  obs_properties_destroy(props);
  obs_source_release(source);
}

static void on_frontend_event(enum obs_frontend_event event, void *) {
  switch (event) {
  case OBS_FRONTEND_EVENT_FINISHED_LOADING:
    if (!g_buttons)
      g_buttons = ContextButtons::attach(static_cast<QWidget *>(obs_frontend_get_main_window()));
    break;
  case OBS_FRONTEND_EVENT_EXIT:
    if (g_buttons)
      g_buttons->stop();
    break;
  default:
    break;
  }
}

void init_context_buttons() {
  obs_frontend_add_event_callback(on_frontend_event, nullptr);
}

void shutdown_context_buttons() {
  obs_frontend_remove_event_callback(on_frontend_event, nullptr);
  g_buttons = nullptr;
}

} // namespace lss
