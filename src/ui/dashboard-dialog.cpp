#include "ui/dashboard-dialog.h"
#include "network/ws-stats-server.h"
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVBoxLayout>
#include <obs-frontend-api.h>
#include <obs-module.h>

namespace lss {

StatsWidget::StatsWidget(QWidget *parent) : QWidget(parent) {
  setupUI();

  refreshTimer = new QTimer(this);
  connect(refreshTimer, &QTimer::timeout, this, &StatsWidget::refreshStats);
  refreshTimer->start(1000);
}

void StatsWidget::setupUI() {
  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  table = new QTableWidget(this);
  table->setColumnCount(10);
  table->setHorizontalHeaderLabels(
      {QString::fromUtf8(obs_module_text("ColSourceName")),
       QString::fromUtf8(obs_module_text("ColStatus")),
       QString::fromUtf8(obs_module_text("ColResolution")),
       QString::fromUtf8(obs_module_text("ColBitrate")),
       QString::fromUtf8(obs_module_text("ColFPS")),
       QString::fromUtf8(obs_module_text("ColLatency")),
       QString::fromUtf8(obs_module_text("ColDelay")),
       QString::fromUtf8(obs_module_text("ColDropped")),
       QString::fromUtf8(obs_module_text("ColHWAccel")),
       QString::fromUtf8(obs_module_text("ColUptime"))});

  table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setAlternatingRowColors(true);

  layout->addWidget(table);

  refreshStats();
}

static QString fmt_uptime(int s) {
  if (s < 60)
    return QString::number(s) + "s";
  if (s < 3600)
    return QString::number(s / 60) + "m " + QString::number(s % 60) + "s";
  return QString::number(s / 3600) + "h " + QString::number((s % 3600) / 60) +
         "m";
}

void StatsWidget::refreshStats() {
  auto all_stats = WsStatsServer::instance().get_all_stats();
  updateTable(all_stats);
}

void StatsWidget::updateTable(
    const std::map<std::string, std::string> &all_stats) {
  table->setRowCount((int)all_stats.size());

  int row = 0;
  for (auto const &[name, json_str] : all_stats) {
    QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray::fromStdString(json_str));
    QJsonObject obj = doc.object();

    bool connected = obj["connected"].toBool();

    table->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(name)));

    QTableWidgetItem *statusItem = new QTableWidgetItem(
        connected ? QString::fromUtf8(obs_module_text("StatConnected"))
                  : QString::fromUtf8(obs_module_text("StatDisconnected")));
    if (connected)
      statusItem->setForeground(Qt::green);
    else
      statusItem->setForeground(Qt::red);
    table->setItem(row, 1, statusItem);

    QString res =
        QString("%1x%2").arg(obj["width"].toInt()).arg(obj["height"].toInt());
    table->setItem(row, 2, new QTableWidgetItem(res));

    QString bitrate = QString("%1 kbps").arg(obj["kbps"].toInt());
    table->setItem(row, 3, new QTableWidgetItem(bitrate));

    table->setItem(
        row, 4,
        new QTableWidgetItem(QString::number(obj["fps"].toDouble(), 'f', 2)));
    table->setItem(
        row, 5,
        new QTableWidgetItem(QString("%1 ms").arg(obj["latency_ms"].toInt())));
    table->setItem(row, 6,
                   new QTableWidgetItem(
                       QString("%1 ms").arg(obj["stream_delay_ms"].toInt())));
    table->setItem(
        row, 7,
        new QTableWidgetItem(QString::number(obj["dropped_frames"].toInt())));
    table->setItem(row, 8,
                   new QTableWidgetItem(
                       obj["hw_accel"].toBool()
                           ? QString::fromUtf8(obs_module_text("StatActive"))
                           : QString::fromUtf8(obs_module_text("StatOff"))));
    table->setItem(row, 9,
                   new QTableWidgetItem(fmt_uptime(obj["uptime_s"].toInt())));

    row++;
  }
}

} // namespace lss
