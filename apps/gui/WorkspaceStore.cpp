#include "WorkspaceStore.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <stdexcept>
namespace {
QJsonArray readArray(const QString &path, qint64 limit) {
    QFile f(path);
    if (!f.exists())
        return {};
    if (!f.open(QIODevice::ReadOnly) || f.size() > limit)
        throw std::runtime_error("record file unreadable or oversized");
    QJsonParseError error;
    auto d = QJsonDocument::fromJson(f.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !d.isArray())
        throw std::runtime_error("invalid record file");
    return d.array();
}
bool writeArray(const QString &path, const QJsonArray &a) {
    QSaveFile f(path);
    auto data = QJsonDocument(a).toJson(QJsonDocument::Compact);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit();
}
quint64 integer(const QJsonObject &o, const QString &key) {
    const auto v = o.value(key);
    bool ok = false;
    auto text = v.toString();
    auto n = text.toULongLong(&ok);
    if (!v.isString() || !ok || text != QString::number(n))
        throw std::runtime_error("invalid integer");
    return n;
}
bool digest(const QString &s, int n) {
    return s.size() == n && QRegularExpression("^[0-9a-f]+$").match(s).hasMatch();
}
} // namespace
WorkspaceStore::WorkspaceStore(QString directory, QObject *parent) : QObject(parent) {
    directory_ = directory.isEmpty()
                     ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                     : directory;
    logDirectory_ = QDir(directory_).filePath("logs");
    QDir().mkpath(directory_);
    QDir().mkpath(logDirectory_);
    try {
        auto a = readArray(QDir(directory_).filePath("history.json"), 4 * 1024 * 1024);
        if (a.size() > 500)
            throw std::runtime_error("too many records");
        QList<gui::FileRecord> loaded;
        for (auto v : a) {
            if (!v.isObject())
                throw std::runtime_error("invalid history row");
            auto o = v.toObject();
            gui::FileRecord f;
            f.localPath = o["local"].toString();
            f.logicalPath = o["logical"].toString();
            f.versionId = o["version"].toString();
            f.sha256 = o["sha256"].toString();
            f.baseVersion = o["base"].toString();
            f.time = o["time"].toString();
            f.endpoint = o["endpoint"].toString();
            f.status = "已完成";
            f.change = o["change"].toString();
            f.note = o["note"].toString();
            f.size = integer(o, "size");
            f.changedBlocks = integer(o, "blocks");
            f.missingBytes = integer(o, "bytes");
            if (!digest(f.versionId, 32) || !digest(f.sha256, 64) || f.logicalPath.isEmpty() ||
                f.logicalPath.size() > 4096 ||
                !QDateTime::fromString(f.time, Qt::ISODate).isValid())
                throw std::runtime_error("invalid history fields");
            loaded.append(f);
        }
        records_ = loaded;
    } catch (const std::exception &) {
        // Keep corrupt input for diagnosis instead of overwriting it with an empty success list.
        auto path = QDir(directory_).filePath("history.json");
        QFile::copy(path,
                    path + ".invalid-" + QString::number(QDateTime::currentMSecsSinceEpoch()));
        appendLog("警告", "本机历史记录损坏或超过上限，未加载；原记录已保留。");
    }
    setLogDirectory(logDirectory_);
}
void WorkspaceStore::saveHistory() {
    QJsonArray a;
    for (const auto &f : records_)
        a.append(QJsonObject{{"local", f.localPath},
                             {"logical", f.logicalPath},
                             {"version", f.versionId},
                             {"sha256", f.sha256},
                             {"base", f.baseVersion},
                             {"time", f.time},
                             {"endpoint", f.endpoint},
                             {"change", f.change},
                             {"note", f.note},
                             {"size", QString::number(f.size)},
                             {"blocks", QString::number(f.changedBlocks)},
                             {"bytes", QString::number(f.missingBytes)}});
    if (!writeArray(QDir(directory_).filePath("history.json"), a))
        emit storageError("本机历史记录保存失败；远端提交结果不受影响。");
}
void WorkspaceStore::addCommit(const gui::FileRecord &f) {
    if (f.versionId.isEmpty())
        return;
    for (const auto &old : records_)
        if (old.endpoint == f.endpoint && old.versionId == f.versionId)
            return;
    records_.prepend(f);
    while (records_.size() > 500)
        records_.removeLast();
    saveHistory();
    emit changed();
}
void WorkspaceStore::setLogDirectory(const QString &path) {
    auto target = QDir::cleanPath(path);
    if (target.isEmpty() || !QDir::isAbsolutePath(target) || !QDir().mkpath(target)) {
        emit storageError("日志目录不可用。");
        return;
    }
    logDirectory_ = target;
    try {
        auto a = readArray(QDir(logDirectory_).filePath("activity.json"), 2 * 1024 * 1024);
        if (a.size() > 500)
            throw std::runtime_error("too many logs");
        QList<LogEntry> loaded;
        for (auto v : a) {
            auto o = v.toObject();
            if (!v.isObject() || !o["message"].isString() || !o["level"].isString() ||
                !o["time"].isString())
                throw std::runtime_error("invalid log");
            loaded.append({o["level"].toString(), o["time"].toString(), o["message"].toString()});
        }
        if (!loaded.isEmpty())
            logs_ = loaded;
    } catch (const std::exception &) {
        emit storageError("本机日志记录无法读取。");
    }
    emit logsChanged();
}
void WorkspaceStore::appendLog(const QString &level, const QString &message) {
    auto clean = message.left(2048);
    clean.replace('\n', ' ');
    clean.replace('\r', ' ');
    logs_.prepend({level, QDateTime::currentDateTimeUtc().toString(Qt::ISODate), clean});
    while (logs_.size() > 500)
        logs_.removeLast();
    QJsonArray a;
    for (const auto &l : logs_)
        a.append(QJsonObject{{"level", l.level}, {"time", l.time}, {"message", l.message}});
    if (!writeArray(QDir(logDirectory_).filePath("activity.json"), a))
        emit storageError("本机日志保存失败。");
    emit logsChanged();
}
