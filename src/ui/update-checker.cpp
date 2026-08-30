// update-checker.cpp - GitHub release update notification
#include "ui/update-checker.h"
#include "core/common.h"

#include <obs-frontend-api.h>
#include <util/config-file.h>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

#include <QCoreApplication>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include <QWidget>

#include <sstream>
#include <thread>
#include <vector>

namespace lss {
namespace {

constexpr const char *kReleaseApi =
    "https://api.github.com/repos/xacnio/live-stream-source/releases/latest";
constexpr const char *kReleasePage =
    "https://github.com/xacnio/live-stream-source/releases/latest";

constexpr const char *kCfgSection = "LSSUpdate";
constexpr const char *kCfgOnStartup = "CheckOnStartup";
constexpr const char *kCfgSkipVersion = "SkipVersion";

// Let the main window settle before a dialog can pop up.
constexpr int kStartupDelayMs = 3000;
constexpr const char *kTimeoutUs = "10000000"; // 10s

std::atomic<bool> g_abort{false};
std::atomic<bool> g_busy{false};
std::thread g_worker;
bool g_startup_done = false;

QString tr(const char *key) { return QString::fromUtf8(obs_module_text(key)); }

config_t *cfg() { return obs_frontend_get_profile_config(); }

int interrupt_cb(void *) { return g_abort.load() ? 1 : 0; }

// OBS ships no Qt TLS backend, so HTTPS goes through FFmpeg's avio — the
// same stack the LL-HLS fetcher uses.
bool http_get(const char *url, std::string &out) {
  AVDictionary *opts = nullptr;
  av_dict_set(&opts, "user_agent", "live-stream-source/" PLUGIN_VERSION, 0);
  av_dict_set(&opts, "headers", "Accept: application/vnd.github+json\r\n", 0);
  av_dict_set(&opts, "timeout", kTimeoutUs, 0);

  AVIOInterruptCB int_cb{interrupt_cb, nullptr};
  AVIOContext *ctx = nullptr;
  int ret = avio_open2(&ctx, url, AVIO_FLAG_READ, &int_cb, &opts);
  av_dict_free(&opts);

  if (ret < 0) {
    char err[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err, sizeof(err));
    lss_log_warn("Update check: request failed (%s)", err);
    return false;
  }

  std::ostringstream ss;
  std::vector<uint8_t> buf(16384);
  while (!g_abort.load()) {
    int n = avio_read(ctx, buf.data(), static_cast<int>(buf.size()));
    if (n <= 0)
      break;
    ss.write(reinterpret_cast<const char *>(buf.data()), n);
  }
  avio_closep(&ctx);

  out = ss.str();
  return !out.empty();
}

// "v1.2.10-beta" -> {1, 2, 10}; a pre-release tag compares equal to its release.
QVector<int> parse_version(QString tag) {
  tag = tag.trimmed();
  if (tag.startsWith('v', Qt::CaseInsensitive))
    tag.remove(0, 1);
  const int cut = tag.indexOf(QRegularExpression("[^0-9.]"));
  if (cut >= 0)
    tag.truncate(cut);

  QVector<int> out;
  const QStringList parts = tag.split('.', Qt::SkipEmptyParts);
  for (const QString &p : parts)
    out.append(p.toInt());
  return out;
}

bool is_newer(const QString &tag, const QString &current) {
  const QVector<int> a = parse_version(tag), b = parse_version(current);
  for (int i = 0; i < qMax(a.size(), b.size()); ++i) {
    const int x = i < a.size() ? a[i] : 0;
    const int y = i < b.size() ? b[i] : 0;
    if (x != y)
      return x > y;
  }
  return false;
}

QString skipped_version() {
  config_t *c = cfg();
  if (!c)
    return QString();
  const char *v = config_get_string(c, kCfgSection, kCfgSkipVersion);
  return v ? QString::fromUtf8(v) : QString();
}

void set_skipped_version(const QString &tag) {
  config_t *c = cfg();
  if (!c)
    return;
  config_set_string(c, kCfgSection, kCfgSkipVersion, tag.toUtf8().constData());
  config_save(c);
}

void show_update_dialog(QWidget *parent, const QString &tag, const QString &url,
                        const QString &notes, bool offer_skip) {
  QMessageBox box(parent);
  box.setIcon(QMessageBox::Information);
  box.setWindowTitle(tr("UpdateTitle"));
  box.setText(tr("UpdateAvailable").arg(tag, QString(PLUGIN_VERSION)));
  box.setInformativeText(tr("UpdateManualHint"));
  if (!notes.isEmpty())
    box.setDetailedText(notes);

  QPushButton *download =
      box.addButton(tr("UpdateDownload"), QMessageBox::AcceptRole);
  QPushButton *skip =
      offer_skip ? box.addButton(tr("UpdateSkip"), QMessageBox::DestructiveRole)
                 : nullptr;
  box.addButton(tr("UpdateLater"), QMessageBox::RejectRole);
  box.setDefaultButton(download);
  box.exec();

  if (box.clickedButton() == download)
    QDesktopServices::openUrl(QUrl(url));
  else if (skip && box.clickedButton() == skip)
    set_skipped_version(tag);
}

// Runs on the UI thread.
void handle_result(bool silent, const QByteArray &body) {
  QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());

  const QJsonObject rel = QJsonDocument::fromJson(body).object();
  const QString tag = rel.value("tag_name").toString();

  if (tag.isEmpty()) {
    lss_log_warn("Update check: no release tag in response");
    if (!silent)
      QMessageBox::warning(parent, tr("UpdateTitle"), tr("UpdateFailed"));
    return;
  }

  if (!is_newer(tag, PLUGIN_VERSION)) {
    lss_log_info("Update check: up to date (latest %s)", qUtf8Printable(tag));
    if (!silent)
      QMessageBox::information(parent, tr("UpdateTitle"),
                               tr("UpdateUpToDate").arg(PLUGIN_VERSION));
    return;
  }

  if (silent && tag == skipped_version()) {
    lss_log_info("Update %s available but skipped by user", qUtf8Printable(tag));
    return;
  }

  lss_log_info("Update available: %s (running %s)", qUtf8Printable(tag),
               PLUGIN_VERSION);
  show_update_dialog(parent, tag, rel.value("html_url").toString(kReleasePage),
                     rel.value("body").toString().trimmed(), silent);
}

void on_frontend_event(enum obs_frontend_event event, void *) {
  if (event == OBS_FRONTEND_EVENT_EXIT) {
    g_abort.store(true);
    return;
  }
  if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING || g_startup_done)
    return;
  g_startup_done = true;
  if (update_check_on_startup_enabled())
    QTimer::singleShot(kStartupDelayMs, []() { check_for_updates(true); });
}

} // namespace

bool update_check_on_startup_enabled() {
  config_t *c = cfg();
  if (!c)
    return true;
  config_set_default_bool(c, kCfgSection, kCfgOnStartup, true);
  return config_get_bool(c, kCfgSection, kCfgOnStartup);
}

void set_update_check_on_startup(bool enabled) {
  config_t *c = cfg();
  if (!c)
    return;
  config_set_bool(c, kCfgSection, kCfgOnStartup, enabled);
  config_save(c);
}

void check_for_updates(bool silent) {
  if (g_abort.load() || g_busy.exchange(true))
    return;
  if (g_worker.joinable())
    g_worker.join();

  g_worker = std::thread([silent]() {
    std::string body;
    const bool ok = http_get(kReleaseApi, body);
    g_busy.store(false);
    if (g_abort.load())
      return;

    // Posted to qApp, not the main window: qApp outlives it, and the queued
    // call simply never runs if the event loop is already gone.
    const QByteArray payload =
        ok ? QByteArray::fromStdString(body) : QByteArray();
    QMetaObject::invokeMethod(
        qApp, [silent, payload]() { handle_result(silent, payload); },
        Qt::QueuedConnection);
  });
}

void init_update_checker() {
  obs_frontend_add_event_callback(on_frontend_event, nullptr);
}

void shutdown_update_checker() {
  g_abort.store(true);
  obs_frontend_remove_event_callback(on_frontend_event, nullptr);
  if (g_worker.joinable())
    g_worker.join();
}

} // namespace lss
