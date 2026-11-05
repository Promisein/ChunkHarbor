#include "SnapshotRestore.h"
#include "SnapshotOutput.h"
#include "SnapshotPreflight.h"
#include "DownloadTransfer.h"
#include <QThread>
#include <mutex>
#include <stdexcept>

namespace gui {
using namespace chunkharbor::protocol;
struct SnapshotRestoreJob {
    // Only the transition from Running can win. A rename already in progress has a real result.
    enum Phase { Running, Cancelled, Publishing };
    std::atomic<Phase> phase{Running};
    std::atomic_bool cancelled{false};
    std::mutex mutex;
    SnapshotRestoreProgress latest;
    bool pending = false;
};

SnapshotRestore::SnapshotRestore(QObject *parent) : QObject(parent) {}
SnapshotRestore::~SnapshotRestore() {
    cancel();
    if (worker_.joinable()) worker_.join();
}
void SnapshotRestore::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!busy_) return;
    auto running = SnapshotRestoreJob::Running;
    if (job_->phase.compare_exchange_strong(running, SnapshotRestoreJob::Cancelled)) {
        job_->cancelled = true;
        progress_.status = tr("正在取消恢复并清理暂存目录…");
        emit changed();
    }
}

bool SnapshotRestore::start(QString host, quint16 port, SnapshotInfo snapshot,
    std::shared_ptr<const Manifest> manifest, QString target) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (busy_ || !manifest || manifest->size() != snapshot.fileCount || manifest->size() > MaxSnapshotFiles ||
        host.isEmpty() || !port || target.isEmpty()) return false;
    if (worker_.joinable()) worker_.join();
    quint64 size = 0;
    for (const auto &entry : *manifest) {
        if (entry.size > MaxDownloadFileSize) return false;
        size += entry.size;
    }
    job_ = std::make_shared<SnapshotRestoreJob>();
    busy_ = true;
    progress_ = {};
    progress_.count = manifest->size();
    progress_.size = size;
    progress_.status = tr("正在复核快照 %1 的恢复目标…").arg(QString::fromStdString(snapshot.id));
    try {
        worker_ = std::thread([this, job = job_, host, port, snapshot = std::move(snapshot),
            manifest = std::move(manifest), target, progress = progress_]() mutable {
            auto report = [&] {
                std::lock_guard lock(job->mutex);
                job->latest = progress;
                if (job->pending) return;
                job->pending = true;
                QMetaObject::invokeMethod(this, [this, job] {
                    {
                        std::lock_guard lock(job->mutex);
                        job->pending = false;
                        if (job_ != job || !busy_ || job->phase == SnapshotRestoreJob::Cancelled) return;
                        progress_ = job->latest;
                    }
                    emit changed();
                }, Qt::QueuedConnection);
            };
            auto check = [&] {
                if (job->phase == SnapshotRestoreJob::Cancelled) throw std::runtime_error("cancelled");
            };
            std::unique_ptr<SnapshotOutput> output;
            try {
                const auto preflight = preflightSnapshot(target, *manifest, job->cancelled, true);
                check();
                if (!preflight.rootError.isEmpty()) throw std::runtime_error(preflight.rootError.toStdString());
                for (std::size_t i = 0; i < preflight.rows.size(); ++i) {
                    const auto &row = preflight.rows[i];
                    if (!row.pathError.isEmpty() || !row.conflict.isEmpty())
                        throw std::runtime_error((QString::fromUtf8((*manifest)[i].path) + " · " +
                            row.pathError + " " + row.conflict).toStdString());
                }
                output = std::make_unique<SnapshotOutput>(target);
                quint64 verifiedBytes = 0;
                for (const auto &entry : *manifest) {
                    check();
                    progress.file = QString::fromUtf8(entry.path);
                    progress.fileSize = entry.size;
                    progress.fileBytes = 0;
                    progress.fileFlushed = false;
                    progress.status = tr("正在按快照下载和校验文件…");
                    report();
                    output->openFile(progress.file, entry.size);
                    auto remote = entry;
                    remote.path = snapshot.root + '/' + entry.path;
                    receiveDownload(host, port, remote, DownloadSelection::Historical, check,
                        [&](std::string_view bytes) { output->write(bytes); },
                        [&](quint64 offset) {
                            progress.fileBytes = offset;
                            progress.bytes = verifiedBytes + offset;
                            report();
                        });
                    check();
                    output->flushFile();
                    progress.fileFlushed = true;
                    verifiedBytes += entry.size;
                    ++progress.files;
                    report();
                }
                progress.status = tr("正在核对完整暂存目录…");
                report();
                output->verify(check);
                auto running = SnapshotRestoreJob::Running;
                if (!job->phase.compare_exchange_strong(running, SnapshotRestoreJob::Publishing)) check();
                progress.status = tr("正在发布完整目录…");
                report();
                output->publish();
                progress.published = true;
                progress.status = tr("恢复成功。快照 %1 已发布到 %2")
                    .arg(QString::fromStdString(snapshot.id), target);
            } catch (const std::exception &error) {
                progress.status = job->phase == SnapshotRestoreJob::Cancelled ? tr("恢复已取消。")
                    : tr("恢复失败。%1").arg(QString::fromUtf8(error.what()));
                if (output) {
                    progress.residue = output->cleanup();
                    if (!progress.residue.isEmpty())
                        progress.status += tr(" 清理失败，暂存目录仍在 %1").arg(progress.residue);
                }
            }
            output.reset();
            QMetaObject::invokeMethod(this, [this, job, progress = std::move(progress)] {
                if (job_ != job) return;
                progress_ = progress;
                busy_ = false;
                emit changed();
                emit finished();
            }, Qt::QueuedConnection);
        });
    } catch (const std::exception &error) {
        busy_ = false;
        progress_.status = tr("无法启动恢复。%1").arg(QString::fromUtf8(error.what()));
    }
    emit changed();
    return true;
}
}
