#pragma once
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>

namespace gui {
struct BackupConfig {
    QString directory, prefix, stateDirectory;
    QString host = "127.0.0.1";
    bool direct = false;
    quint16 port = 18787;
    quint32 chunkSize = 262144;
    QString mode = "auto";
    QStringList exclusions = {".git", "build"};
    bool resume = true; // Auto/resume always validate sessions; this switch controls manual full/incremental.
};
enum class BackupAdvice { Unknown, Full, Incremental, Resume, Unchanged };
enum class BackupStrategy { Error, Full, Incremental, Resume, Skip };
struct FileRecord {
    QString localPath, logicalPath, status, change, note, sha256, versionId, baseVersion, time;
    QString endpoint;
    quint64 size = 0, missingBytes = 0, changedBlocks = 0;
    BackupAdvice advice = BackupAdvice::Unknown; // Ephemeral, derived from live server evidence.
    bool modeAllowed = true;
    QString resumeSession;
    BackupStrategy strategy = BackupStrategy::Error; // Confirmed action, never persisted as a receipt.
};
struct TransferProgress {
    QString file, phase;
    quint64 acknowledgedBytes = 0, totalBytes = 0;
    int completedFiles = 0, totalFiles = 0;
    double bytesPerSecond = 0;
};
struct RunControl {
    std::atomic_bool cancelled{false}, paused{false};
    std::mutex mutex;
    std::condition_variable changed;
    void cancel() {
        cancelled = true;
        changed.notify_all();
    }
    bool checkpoint() {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, std::chrono::milliseconds(100),
                         [this] { return !paused.load() || cancelled.load(); });
        while (paused && !cancelled)
            changed.wait_for(lock, std::chrono::milliseconds(100));
        return cancelled;
    }
};
struct WorkerCallbacks {
    std::function<void(const FileRecord &)> entry;
    std::function<void(const TransferProgress &)> progress;
    std::function<void(const QString &, const QString &)> log;
};
struct RunResult {
    int succeeded = 0, failed = 0, skipped = 0;
    bool cancelled = false;
    bool planComplete = false; // A complete scan may contain isolated file errors.
};
RunResult runBackup(const BackupConfig &, bool preview, const QList<FileRecord> &pinned,
                    RunControl &, const WorkerCallbacks &);
QString formatBytes(quint64 bytes);
QString modeRecommendation(const QList<FileRecord> &, bool automatic = false);
QString strategyName(BackupStrategy);
} // namespace gui
Q_DECLARE_METATYPE(gui::FileRecord)
Q_DECLARE_METATYPE(gui::TransferProgress)
Q_DECLARE_METATYPE(gui::RunResult)
