#pragma once

#include <QTableWidget>
#include <QTimer>
#include <QWidget>
#include <map>
#include <string>

namespace lss {

class StatsWidget : public QWidget {
  Q_OBJECT

public:
  StatsWidget(QWidget *parent = nullptr);
  ~StatsWidget() = default;

private slots:
  void refreshStats();

private:
  void setupUI();
  void updateTable(const std::map<std::string, std::string> &all_stats);

  QTableWidget *table;
  QTimer *refreshTimer;
};

} // namespace lss
