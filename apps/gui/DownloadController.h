#pragma once
#include "chunkharbor/protocol/FilesList.h"
#include "chunkharbor/protocol/Download.h"
#include <QObject>
#include <memory>
#include <thread>

struct DownloadControl;
class DownloadController final : public QObject {
    Q_OBJECT
  public:
    explicit DownloadController(QObject *parent = nullptr);
    ~DownloadController() override;
    bool start(QString host, quint16 port, chunkharbor::protocol::ListedFile file, QString target,
               chunkharbor::protocol::DownloadSelection = chunkharbor::protocol::DownloadSelection::Current);
    void cancel();
    bool busy() const { return busy_; }
    bool succeeded() const { return succeeded_; }
    QString status() const { return status_; }
    QString fileName() const { return fileName_; }
    QString historicalVersion() const { return historicalVersion_; }
    quint64 received() const { return received_; }
    quint64 total() const { return total_; }
  signals:
    void changed();
    void finished(bool success);
  private:
    std::thread worker_;
    std::shared_ptr<DownloadControl> control_;
    bool busy_ = false;
    bool succeeded_ = false;
    QString status_ = tr("尚未开始下载。"), fileName_, historicalVersion_;
    quint64 received_ = 0, total_ = 0;
};
