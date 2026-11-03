#pragma once
#include "TaskModel.h"
#include <QObject>
#include <memory>
#include <thread>
class WorkspaceStore;
class TransferController final : public QObject {
    Q_OBJECT
  public:
    explicit TransferController(WorkspaceStore *, QObject *parent = nullptr);
    ~TransferController() override;
    TaskModel *previewModel() const { return model_; }
    bool busy() const { return busy_; }
    bool hasPreview() const { return previewReady_; }
    bool paused() const;
    QString stateDirectory() const;
    void scan(gui::BackupConfig);
    void invalidatePreview();
    QString recommendation() const { return recommendation_; }
    void start(gui::BackupConfig);
    void retry();
    void pauseOrResume();
    void cancel();
  signals:
    void busyChanged(bool);
    void previewReady(bool);
    void progressChanged(const gui::TransferProgress &);
    void finished(const QString &summary, bool success);
    void pausedChanged(bool);
    void recommendationChanged(const QString &);

  private:
    void launch(gui::BackupConfig, bool preview);
    WorkspaceStore *store_;
    TaskModel *model_;
    bool busy_ = false, previewReady_ = false;
    gui::BackupConfig lastConfig_, previewConfig_;
    QList<gui::FileRecord> pinned_;
    QString recommendation_ = "扫描后将依据服务器版本和会话给出建议。";
    std::shared_ptr<gui::RunControl> control_;
    std::thread worker_;
};
