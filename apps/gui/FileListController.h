#pragma once
#include "QtTcpTransport.h"
#include "ServerFilesModel.h"
#include <deque>
class FileListController final : public QObject {
    Q_OBJECT
  public:
    explicit FileListController(QObject *parent = nullptr);
    ServerFilesModel *model() { return &model_; }
    bool busy() const { return busy_; }
    bool hasNext() const { return !model_.page().nextAfterPath.empty(); }
    bool hasPrevious() const { return !history_.empty(); }
    quint64 pageNumber() const { return pageNumber_; }
    QString status() const { return status_; }
    QString endpoint() const { return QString("%1:%2").arg(host_).arg(port_); }
    QString host() const { return host_; }
    quint16 port() const { return port_; }
    void setEndpoint(const QString &, quint16);
    void refresh();
    void next();
    void previous();
    void cancel();
  signals:
    void changed();
    void finished(bool success);
  private:
    enum class Action { Refresh, Next, Previous };
    void start(Action, std::string cursor);
    void fail(const QString &);
    QtTcpTransport transport_;
    ServerFilesModel model_;
    QString host_ = "127.0.0.1", status_;
    quint16 port_ = 18787;
    bool busy_ = false, hasPage_ = false;
    quint64 pageNumber_ = 1;
    Action action_ = Action::Refresh;
    std::string cursor_, pendingCursor_;
    std::deque<std::string> history_;
};
