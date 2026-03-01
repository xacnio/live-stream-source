#pragma once

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QTabWidget>
#include <QTimer>
#include <QWidget>
#include <obs-module.h>
#include <string>

namespace lss {

void init_plugin_settings();
void show_plugin_settings();
void load_and_apply_config();
int get_plugin_port();
std::string get_plugin_bind_ip();

class SettingsWidget : public QWidget {
  Q_OBJECT

public:
  SettingsWidget(QWidget *parent = nullptr);
  void saveSettings();

private slots:
  void refreshStatus();

private:
  QLineEdit *portEdit;
  QComboBox *interfaceCombo;
  QLabel *statusLabel;
  QTimer *refreshTimer;
};

class AboutWidget : public QWidget {
  Q_OBJECT

public:
  AboutWidget(QWidget *parent = nullptr);
};

class PluginDialog : public QDialog {
  Q_OBJECT

public:
  static PluginDialog *instance();
  static void show_instance();

private slots:
  void saveAndClose();

private:
  PluginDialog(QWidget *parent = nullptr);

  QTabWidget *tabs;
  SettingsWidget *settingsWidget;
};

} // namespace lss
