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
    QString serverHost() const;
    void setLocalPort(quint16 value);
    QString startupPage() const;
    QString logDirectory() const;
  signals:
    void preferencesChanged();
    void connectionChanged();

  private:
    QComboBox *chunk_, *startup_, *connectionMode_;
    QCheckBox *resume_;
    QLineEdit *exclusions_, *logDirectory_;
};
