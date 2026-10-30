// sync-notice.cpp - brief, non-blocking messages for the operator
#include "ui/sync-notice.h"
#include "core/common.h"

#include <obs-frontend-api.h>

#include <QMainWindow>
#include <QMetaObject>
#include <QStatusBar>
#include <QString>

namespace lss {

void show_status_notice(const std::string &text) {
  lss_log_info("%s", text.c_str());

  auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
  if (!window)
    return;

  const QString message = QString::fromStdString(text);
  QMetaObject::invokeMethod(
      window,
      [window, message]() {
        window->statusBar()->showMessage(message, STATUS_NOTICE_MS);
      },
      Qt::QueuedConnection);
}

} // namespace lss
