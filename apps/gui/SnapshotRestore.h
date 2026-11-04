#pragma once
#include "chunkharbor/protocol/DirectorySnapshot.h"
#include <QObject>
#include <memory>
#include <thread>

namespace gui {
struct SnapshotRestoreJob;
struct SnapshotRestoreProgress {
    QString file, status, residue;
    quint64 fileBytes = 0, fileSize = 0, bytes = 0, size = 0;
    std::size_t files = 0, count = 0;
    bool fileFlushed = false, published = false;
};
class SnapshotRestore final : public QObject {
    Q_OBJECT
  public:
    using Manifest = std::vector<chunkharbor::protocol::ListedFile>;
    explicit SnapshotRestore(QObject *parent = nullptr);
    ~SnapshotRestore() override;
    bool start(QString host, quint16 port, chunkharbor::protocol::SnapshotInfo snapshot,
        std::shared_ptr<const Manifest>, QString target);
    void cancel();
    bool busy() const { return busy_; }
    const SnapshotRestoreProgress &progress() const { return progress_; }
  signals:
    void changed();
    void finished();
  private:
    std::thread worker_;
    std::shared_ptr<SnapshotRestoreJob> job_;
    SnapshotRestoreProgress progress_;
    bool busy_ = false;
};
}
