#pragma once
#include "TransferTypes.h"
#include "SshTunnelManager.h"
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
    QPushButton *disconnectButton;
    QLabel *status;
    gui::BackupConfig backupConfig() const;
    QString serverHost() const;
    bool isSshMode() const;
    SshTunnelConfig sshConfig() const;
    void setEndpointAvailable(bool value) { endpointAvailable_ = value; }
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
    QLineEdit *sshHost_, *sshUser_, *sshTarget_;
    QSpinBox *sshPort_, *sshTargetPort_;
    bool endpointAvailable_ = true;
};
