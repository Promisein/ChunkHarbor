#pragma once
#include <QMainWindow>
class QtTcpTransport;
class SettingsPage;
class TransferController;
class WorkspaceStore;
class FileListController;
class DownloadController;
class SnapshotPage;
class QCloseEvent;
class QTimer;
class SshTunnelManager;
struct SshTunnelRuntime;
class MainWindow final : public QMainWindow {
  public:
    enum class StartupMode { Automatic, Manual };
    explicit MainWindow(QWidget *parent = nullptr, QString dataDirectory = {});
    MainWindow(QWidget *, QString dataDirectory, StartupMode);
    MainWindow(QWidget *, QString dataDirectory, SshTunnelRuntime, StartupMode = StartupMode::Automatic);
    ~MainWindow() override;
    QtTcpTransport *transport() const { return transport_; }
    void connectToLocalServer(quint16 port);
    TransferController *controller() const { return controller_; }
    WorkspaceStore *store() const { return store_; }
    FileListController *fileListController() const { return fileList_; }
    DownloadController *downloadController() const { return download_; }
    SshTunnelManager *tunnelManager() const { return tunnel_; }

  protected:
    void closeEvent(QCloseEvent *) override;

  private:
    QtTcpTransport *transport_;
    SettingsPage *settings_;
    WorkspaceStore *store_;
    TransferController *controller_;
    FileListController *fileList_;
    DownloadController *download_;
    SnapshotPage *snapshots_;
    SshTunnelManager *tunnel_;
    QTimer *connectionTimer_;
    bool directProbePending_ = false;
    bool userDisconnected_ = false;
    StartupMode startupMode_ = StartupMode::Automatic;
    bool closing_ = false;
};
