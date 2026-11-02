#include "TransferController.h"
#include "WorkspaceStore.h"
#include <QDir>
#include <QMetaObject>
namespace {
bool sameConfiguration(const gui::BackupConfig &a, const gui::BackupConfig &b) {
    return QDir::cleanPath(a.directory) == QDir::cleanPath(b.directory) && a.prefix == b.prefix &&
           a.port == b.port && a.chunkSize == b.chunkSize && a.mode == b.mode &&
           a.exclusions == b.exclusions && a.resume == b.resume;
}
} // namespace
TransferController::TransferController(WorkspaceStore *store, QObject *parent)
    : QObject(parent), store_(store), model_(new TaskModel(this)) {
    qRegisterMetaType<gui::FileRecord>();
    qRegisterMetaType<gui::TransferProgress>();
}
TransferController::~TransferController() {
    if (control_)
        control_->cancel();
    if (worker_.joinable())
        worker_.join();
}
QString TransferController::stateDirectory() const {
    return QDir(store_->directory()).filePath("sessions");
}
bool TransferController::paused() const { return control_ && control_->paused; }
void TransferController::scan(gui::BackupConfig config) {
    if (busy_)
        return;
    model_->clear();
    pinned_.clear();
    previewReady_ = false;
    emit previewReady(false);
    launch(std::move(config), true);
}
void TransferController::start(gui::BackupConfig config) {
    if (busy_)
        return;
    if (!previewReady_ || !sameConfiguration(config, previewConfig_)) {
        auto msg = tr("请先按当前目录、模式和连接设置完成差异预览。");
        store_->appendLog("警告", msg);
        emit finished(msg, false);
        return;
    }
    launch(std::move(config), false);
}
void TransferController::retry() {
    if (busy_)
        return;
    launch(lastConfig_, lastPreview_);
}
void TransferController::pauseOrResume() {
    if (!busy_ || !control_)
        return;
    bool pause = !control_->paused.load();
    control_->paused = pause;
    control_->changed.notify_all();
    store_->appendLog("信息",
                      pause ? "已请求暂停，将在当前操作的安全边界生效。" : "继续备份任务。");
    emit pausedChanged(pause);
}
void TransferController::cancel() {
    if (!busy_ || !control_)
        return;
    control_->cancel();
    store_->appendLog("警告", "正在取消任务；已持久确认的上传进度保留以供续传。");
}
void TransferController::launch(gui::BackupConfig config, bool preview) {
    if (busy_)
        return;
    if (worker_.joinable())
        worker_.join();
    if (config.directory.isEmpty() || !config.port) {
        emit finished(tr("请选择本地目录并设置有效端口。"), false);
        return;
    }
    config.stateDirectory = stateDirectory();
    lastConfig_ = config;
    lastPreview_ = preview;
    busy_ = true;
    control_ = std::make_shared<gui::RunControl>();
    emit busyChanged(true);
    emit pausedChanged(false);
    store_->appendLog("信息",
                      preview ? "开始真实目录扫描和服务端差异预览。" : "开始真实备份任务。");
    auto control = control_;
    auto pinned = preview ? QList<gui::FileRecord>{} : pinned_;
    worker_ = std::thread([this, config, preview, control, pinned] {
        gui::WorkerCallbacks cb;
        cb.entry = [this, preview](const gui::FileRecord &f) {
            QMetaObject::invokeMethod(
                this,
                [this, preview, f] {
                    model_->upsert(f);
                    if (!f.versionId.isEmpty())
                        store_->addCommit(f);
                },
                Qt::QueuedConnection);
        };
        cb.progress = [this](const gui::TransferProgress &p) {
            QMetaObject::invokeMethod(
                this, [this, p] { emit progressChanged(p); }, Qt::QueuedConnection);
        };
        cb.log = [this](const QString &level, const QString &message) {
            QMetaObject::invokeMethod(
                this, [this, level, message] { store_->appendLog(level, message); },
                Qt::QueuedConnection);
        };
        gui::RunResult result;
        try {
            result = gui::runBackup(config, preview, pinned, *control, cb);
        } catch (const std::exception &e) {
            result.failed = 1;
            result.cancelled = control->cancelled;
            cb.log("错误", tr("任务失败：%1").arg(QString::fromUtf8(e.what())));
        }
        QMetaObject::invokeMethod(
            this,
            [this, config, preview, result] {
                busy_ = false;
                bool success = !result.cancelled && result.failed == 0;
                if (preview) {
                    previewReady_ = success;
                    previewConfig_ = config;
                    if (success)
                        pinned_ = model_->records();
                } else if (success)
                    previewReady_ = false;
                emit busyChanged(false);
                emit pausedChanged(false);
                emit previewReady(previewReady_);
                auto summary = tr("%1：成功 %2，失败 %3，跳过 %4%5")
                                   .arg(preview ? tr("预览结束") : tr("备份结束"))
                                   .arg(result.succeeded)
                                   .arg(result.failed)
                                   .arg(result.skipped)
                                   .arg(result.cancelled ? tr("，已取消，可重试续传") : QString{});
                store_->appendLog(success ? "信息" : "警告", summary);
                emit finished(summary, success);
            },
            Qt::QueuedConnection);
    });
}
