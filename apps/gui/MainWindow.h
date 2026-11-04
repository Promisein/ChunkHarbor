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
class MainWindow final : public QMainWindow {
  public:
    explicit MainWindow(QWidget *parent = nullptr, QString dataDirectory = {});
    ~MainWindow() override;
    QtTcpTransport *transport() const { return transport_; }
    void connectToLocalServer(quint16 port);
    TransferController *controller() const { return controller_; }
    WorkspaceStore *store() const { return store_; }
    FileListController *fileListController() const { return fileList_; }
    DownloadController *downloadController() const { return download_; }

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
    bool closing_ = false;
};
