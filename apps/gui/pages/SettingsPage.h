#pragma once
#include "TransferTypes.h"
#include "WorkspacePage.h"
class QLineEdit;
class QSpinBox;
class QPushButton;
class QLabel;
class QComboBox;
class QCheckBox;
class SettingsPage final : public WorkspacePage {
    Q_OBJECT
  public:
    explicit SettingsPage(QWidget *parent = nullptr);
    QLineEdit *host;
    QSpinBox *port;
    QPushButton *connectButton;
    QLabel *status;
    gui::BackupConfig backupConfig() const;
    QString startupPage() const;
    QString logDirectory() const;
  signals:
    void preferencesChanged();

  private:
    QComboBox *chunk_, *startup_;
    QCheckBox *resume_;
    QLineEdit *exclusions_, *logDirectory_;
};
