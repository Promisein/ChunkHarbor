#pragma once
#include "TransferTypes.h"
#include <QObject>
struct LogEntry {
    QString level, time, message;
};
class WorkspaceStore final : public QObject {
    Q_OBJECT
  public:
    explicit WorkspaceStore(QString directory = {}, QObject *parent = nullptr);
    const QList<gui::FileRecord> &records() const { return records_; }
    const QList<LogEntry> &logs() const { return logs_; }
    QString directory() const { return directory_; }
    QString logDirectory() const { return logDirectory_; }
    void setLogDirectory(const QString &);
    void addCommit(const gui::FileRecord &);
    void appendLog(const QString &level, const QString &message);
  signals:
    void changed();
    void logsChanged();
    void storageError(const QString &);

  private:
    void saveHistory();
    QString directory_, logDirectory_;
    QList<gui::FileRecord> records_;
    QList<LogEntry> logs_;
};
