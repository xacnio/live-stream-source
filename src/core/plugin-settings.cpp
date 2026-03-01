#include "core/plugin-settings.h"
#include "network/ws-stats-server.h"
#include "ui/dashboard-dialog.h"
#include <obs-frontend-api.h>
#include <string>
#include <util/config-file.h>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QNetworkInterface>
#include <QPushButton>
#include <QVBoxLayout>

namespace lss {

static int g_port = 4477;
static std::string g_bind_ip = "127.0.0.1";

int get_plugin_port() { return g_port; }
std::string get_plugin_bind_ip() { return g_bind_ip; }

void load_and_apply_config() {
  config_t *cfg = obs_frontend_get_profile_config();
  if (cfg) {
    g_port = (int)config_get_uint(cfg, "LSSServer", "Port");
    if (g_port <= 0 || g_port > 65535)
      g_port = 4477;

    const char *saved_ip = config_get_string(cfg, "LSSServer", "BindIP");
    if (saved_ip && *saved_ip) {
      g_bind_ip = saved_ip;
    } else {
      // Migrate from old boolean config
      bool bind_any = config_get_bool(cfg, "LSSServer", "BindAny");
      g_bind_ip = bind_any ? "0.0.0.0" : "127.0.0.1";
    }
  } else {
    g_port = 4477;
    g_bind_ip = "127.0.0.1";
  }
  WsStatsServer::instance().configure(g_port, g_bind_ip);
}

static void save_config(int port, const std::string &bind_ip) {
  config_t *cfg = obs_frontend_get_profile_config();
  if (cfg) {
    config_set_uint(cfg, "LSSServer", "Port", port);
    config_set_string(cfg, "LSSServer", "BindIP", bind_ip.c_str());
    config_save(cfg);
  }
  g_port = port;
  g_bind_ip = bind_ip;
  WsStatsServer::instance().configure(g_port, g_bind_ip);
}

SettingsWidget::SettingsWidget(QWidget *parent) : QWidget(parent) {
  QVBoxLayout *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(0, 0, 0, 0);

  QFormLayout *formLayout = new QFormLayout();

  portEdit = new QLineEdit(QString::number(g_port));
  portEdit->setValidator(new QIntValidator(1, 65535, this));
  formLayout->addRow(QString::fromUtf8(obs_module_text("WsPort")), portEdit);

  interfaceCombo = new QComboBox();
  interfaceCombo->addItem(QString::fromUtf8(obs_module_text("WsLocalhost")),
                          "127.0.0.1");

  QList<QHostAddress> list = QNetworkInterface::allAddresses();
  for (const QHostAddress &addr : list) {
    if (addr.protocol() == QAbstractSocket::IPv4Protocol &&
        !addr.isLoopback()) {
      interfaceCombo->addItem(addr.toString(), addr.toString());
    }
  }
  interfaceCombo->addItem(QString::fromUtf8(obs_module_text("WsAllInterfaces")),
                          "0.0.0.0");

  int idx = interfaceCombo->findData(QString::fromStdString(g_bind_ip));
  if (idx != -1)
    interfaceCombo->setCurrentIndex(idx);

  formLayout->addRow(QString::fromUtf8(obs_module_text("WsInterface")),
                     interfaceCombo);

  statusLabel = new QLabel();
  refreshStatus();

  mainLayout->addLayout(formLayout);
  mainLayout->addSpacing(10);
  mainLayout->addWidget(statusLabel);
  mainLayout->addStretch();

  refreshTimer = new QTimer(this);
  connect(refreshTimer, &QTimer::timeout, this, &SettingsWidget::refreshStatus);
  refreshTimer->start(1000);
}

void SettingsWidget::refreshStatus() {
  auto &server = WsStatsServer::instance();
  QString statusMsg =
      server.is_running()
          ? QString("<span style='color:green;'>%1</span> (Port: %2, "
                    "%3 %4)")
                .arg(QString::fromUtf8(obs_module_text("WsRunning")))
                .arg(server.get_port())
                .arg(QString::fromUtf8(obs_module_text("WsClients")))
                .arg(server.get_client_count())
          : QString("<span style='color:#a8a8a8;'>%1</span> %2")
                .arg(QString::fromUtf8(obs_module_text("WsStopped")))
                .arg(QString::fromUtf8(obs_module_text("WsRequiresSource")));

  statusLabel->setText(
      QString("<b>%1</b> %2")
          .arg(QString::fromUtf8(obs_module_text("WsStatus")), statusMsg));
}

void SettingsWidget::saveSettings() {
  int p = portEdit->text().toInt();
  if (p <= 0 || p > 65535)
    return;

  std::string ip = interfaceCombo->currentData().toString().toStdString();
  save_config(p, ip);
}

AboutWidget::AboutWidget(QWidget *parent) : QWidget(parent) {
  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  QLabel *title = new QLabel(
      "<h2>" + QString::fromUtf8(obs_module_text("PluginTitle")) + "</h2>");
  QString descText =
      QString("%1<b>v%2</b><br>%3<br><br>%4<br><br>GitHub: <a "
              "href=\"https://github.com/xacnio/"
              "live-stream-source\">https://github.com/xacnio/"
              "live-stream-source</a>")
          .arg(QString::fromUtf8(obs_module_text("PluginVersion")))
          .arg(PLUGIN_VERSION)
          .arg(QString::fromUtf8(obs_module_text("PluginAuthor")))
          .arg(QString::fromUtf8(obs_module_text("PluginDesc")));
  QLabel *desc = new QLabel(descText);
  desc->setWordWrap(true);
  desc->setOpenExternalLinks(true);

  layout->addWidget(title);
  layout->addWidget(desc);
  layout->addStretch();
}

static PluginDialog *g_plugin_dialog = nullptr;

PluginDialog *PluginDialog::instance() {
  if (!g_plugin_dialog) {
    QWidget *main_win = (QWidget *)obs_frontend_get_main_window();
    g_plugin_dialog = new PluginDialog(main_win);
  }
  return g_plugin_dialog;
}

void PluginDialog::show_instance() {
  PluginDialog::instance()->show();
  PluginDialog::instance()->raise();
  PluginDialog::instance()->activateWindow();
}

PluginDialog::PluginDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(QString::fromUtf8(obs_module_text("WndTitle")));
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
  resize(950, 450);

  QVBoxLayout *mainLayout = new QVBoxLayout(this);

  tabs = new QTabWidget(this);

  tabs->setDocumentMode(true);

  StatsWidget *statsWidget = new StatsWidget(this);
  settingsWidget = new SettingsWidget(this);
  AboutWidget *aboutWidget = new AboutWidget(this);

  tabs->addTab(statsWidget,
               QString::fromUtf8(obs_module_text("TabStatistics")));
  tabs->addTab(settingsWidget,
               QString::fromUtf8(obs_module_text("TabSettings")));
  tabs->addTab(aboutWidget, QString::fromUtf8(obs_module_text("TabAbout")));

  QHBoxLayout *btnLayout = new QHBoxLayout();
  QPushButton *saveBtn =
      new QPushButton(QString::fromUtf8(obs_module_text("BtnSaveClose")));
  QPushButton *closeBtn =
      new QPushButton(QString::fromUtf8(obs_module_text("BtnClose")));
  btnLayout->addStretch();
  btnLayout->addWidget(saveBtn);
  btnLayout->addWidget(closeBtn);

  saveBtn->hide();

  connect(tabs, &QTabWidget::currentChanged, this, [saveBtn](int index) {
    if (index == 1) {
      saveBtn->show();
    } else {
      saveBtn->hide();
    }
  });

  mainLayout->addWidget(tabs);
  mainLayout->addLayout(btnLayout);

  connect(saveBtn, &QPushButton::clicked, this, &PluginDialog::saveAndClose);
  connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void PluginDialog::saveAndClose() {
  settingsWidget->saveSettings();
  accept();
}

static void on_menu_action(void *) { PluginDialog::show_instance(); }

void init_plugin_settings() {
  obs_frontend_add_tools_menu_item(obs_module_text("MenuDashboard"),
                                   on_menu_action, nullptr);
  load_and_apply_config();
}

void show_plugin_settings() { on_menu_action(nullptr); }

} // namespace lss
