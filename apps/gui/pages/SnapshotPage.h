#pragma once
#include "WorkspacePage.h"
#include "chunkharbor/protocol/DirectorySnapshot.h"
#include "chunkharbor/protocol/FrameCodec.h"
#include <QPointer>
#include <memory>

class QtTcpTransport;
class QLineEdit;
class QLabel;
class QPushButton;
class QProgressBar;
class QMessageBox;
class QTableWidget;
class QThread;
class SnapshotPlanModel;
struct SnapshotCheckJob;
namespace gui { class SnapshotRestore; }

class SnapshotPage final : public WorkspacePage {
    Q_OBJECT
  public:
    explicit SnapshotPage(QWidget *parent = nullptr);
    ~SnapshotPage() override;
    void setEndpoint(const QString &host, quint16 port);
    bool restoring() const;
    void cancelRestore();
    bool settlingCapture() const;
    void cancelCaptureBeforeSend();
  signals:
    void restoreFinished(const QString &residue);
    void captureSettled(bool uncertain);

  private:
    enum class Request { None, List, Info, Entries, CaptureBaseline, Create, Reconcile };
    struct Capture {
        QString host;
        quint16 port;
        std::uint64_t baseline = 0;
        QString issue;
    };
    void stopRequest();
    void clearPlan();
    void clearList();
    void fail(const QString &message);
    void confirmCapture();
    void cancelQuery();
    void showSnapshots(std::vector<chunkharbor::protocol::SnapshotInfo>);
    QString planHeading() const;
    void queryList(std::optional<std::uint64_t> before);
    void selectSnapshot(int row);
    void connectRequest();
    void sendRequest();
    void receive(const chunkharbor::protocol::Frame &);
    void updateButtons();
    void schedulePreflight();
    void startPreflight();
    QString destination() const;
    void confirmRestore();
    void showRestoreProgress();

    QLineEdit *root_, *target_, *newName_;
    QLabel *status_, *restoreStatus_;
    QPushButton *older_, *cancel_, *previousEntries_, *nextEntries_, *restore_, *restoreCancel_;
    QPushButton *refresh_, *choose_, *captureButton_;
    QProgressBar *fileProgress_, *directoryProgress_;
    gui::SnapshotRestore *restoreJob_;
    QTableWidget *snapshots_;
    SnapshotPlanModel *model_;
    QString host_ = "127.0.0.1";
    quint16 port_ = 18787;
    QtTcpTransport *peer_ = nullptr;
    Request request_ = Request::None;
    std::uint64_t requestId_ = 1;
    std::string rootRequest_, afterPath_;
    std::optional<std::uint64_t> listBefore_, nextBefore_;
    std::vector<chunkharbor::protocol::SnapshotInfo> snapshotRows_;
    std::optional<chunkharbor::protocol::SnapshotInfo> selected_;
    std::optional<Capture> capture_;
    bool currentFolder_ = false;
    std::vector<chunkharbor::protocol::ListedFile> pending_;
    std::shared_ptr<const std::vector<chunkharbor::protocol::ListedFile>> manifest_;
    std::uint64_t total_ = 0;
    std::uint64_t checkRevision_ = 0;
    QThread *worker_ = nullptr;
    std::shared_ptr<SnapshotCheckJob> check_;
    bool preflightReady_ = false;
    QPointer<QMessageBox> confirmation_;
    QString restoreContext_;
};
