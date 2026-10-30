// url-field-guard.cpp
#include "ui/url-field-guard.h"

#include <obs-module.h>

#include <QApplication>
#include <QFormLayout>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>

namespace lss {

// OBS rebuilds the fields a moment after a setting asks it to.
static constexpr int REBUILD_WAIT_MS = 150;

static constexpr const char *GUARDED = "lssGuarded";

// Drawn rather than loaded: OBS's own eye comes in one colour per theme.
static QIcon eye_icon(const QColor &color, bool crossed) {
  QPixmap pixmap(20, 14);
  pixmap.fill(Qt::transparent);

  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(QPen(color, 1.6));

  QPainterPath outline;
  outline.moveTo(1, 7);
  outline.quadTo(10, -3, 19, 7);
  outline.quadTo(10, 17, 1, 7);
  painter.drawPath(outline);
  painter.setBrush(color);
  painter.drawEllipse(QPointF(10, 7), 2.4, 2.4);
  if (crossed)
    painter.drawLine(3, 13, 17, 1);
  return pixmap;
}

// The field and its button share a row of the form, with no name on either.
static QPushButton *button_beside(QLineEdit *edit) {
  auto *form = edit->parentWidget() ? qobject_cast<QFormLayout *>(edit->parentWidget()->layout()) : nullptr;
  if (!form)
    return nullptr;

  for (int row = 0; row < form->rowCount(); ++row) {
    QLayoutItem *field = form->itemAt(row, QFormLayout::FieldRole);
    QLayout *pair = field ? field->layout() : nullptr;
    if (pair && pair->indexOf(edit) >= 0 && pair->count() > 1)
      return qobject_cast<QPushButton *>(pair->itemAt(1)->widget());
  }
  return nullptr;
}

static void guard(QLineEdit *edit) {
  QPushButton *button = button_beside(edit);
  if (!button)
    return;

  edit->setProperty(GUARDED, true);
  edit->setReadOnly(true);

  const QColor color = button->palette().color(QPalette::ButtonText);
  const QIcon reveal = eye_icon(color, false);
  const QIcon conceal = eye_icon(color, true);
  button->setText(QString());
  button->setIcon(reveal);
  button->setIconSize(QSize(20, 14));
  button->setFixedWidth(40);
  button->setToolTip(edit->toolTip());

  // Runs after OBS's own handlers, which swap the echo mode and set the button's text.
  QObject::connect(button, &QPushButton::toggled, edit, [=](bool shown) {
    button->setText(QString());
    button->setIcon(shown ? conceal : reveal);
    // Hiding an emptied field would leave nothing to reveal and nothing to type in.
    edit->setReadOnly(!shown && !edit->text().isEmpty());
    if (shown)
      edit->setFocus();
  });
}

static void guard_open_fields() {
  const QString tooltip = QString::fromUtf8(obs_module_text("StreamURL.Desc"));
  for (QWidget *window : QApplication::topLevelWidgets()) {
    for (QLineEdit *edit : window->findChildren<QLineEdit *>()) {
      if (edit->echoMode() == QLineEdit::Password && edit->toolTip() == tooltip &&
          !edit->property(GUARDED).toBool())
        guard(edit);
    }
  }
}

void protect_url_field() {
  if (!qApp)
    return;
  QMetaObject::invokeMethod(
      qApp,
      [] {
        guard_open_fields();
        QTimer::singleShot(REBUILD_WAIT_MS, qApp, guard_open_fields);
      },
      Qt::QueuedConnection);
}

} // namespace lss
