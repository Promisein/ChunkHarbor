#include "SnapshotPage.h"
#include "QtTcpTransport.h"
#include "SnapshotPreflight.h"
#include "SnapshotRestore.h"
#include "widgets/BrowserUi.h"
#include <QAbstractTableModel>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <algorithm>
#include <QSignalBlocker>
#include <QTableView>
#include <QTableWidget>
#include <QThread>
#include <set>
#include <stdexcept>

using namespace chunkharbor::protocol;
namespace {
constexpr std::uint32_t ListLimit = 32;
constexpr int DisplayPage = 100;
QString creationTime(std::uint64_t seconds) {
    if (seconds > 253402300799ULL) return QObject::tr("时间未知");
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(seconds)).toString("yyyy-MM-dd HH:mm:ss");
}
}

class SnapshotPlanModel final : public QAbstractTableModel {
  public:
    using QAbstractTableModel::QAbstractTableModel;
    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() || !manifest_ ? 0 :
            std::min(DisplayPage, static_cast<int>(manifest_->size()) - offset_);
    }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 7; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 || section >= 7) return {};
        const QStringList columns{tr("相对路径"), tr("版本 ID"), tr("字节数"), tr("SHA-256"),
                                  tr("本地目标"), tr("路径校验"), tr("目标冲突")};
        return columns[section];
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() >= rowCount() || index.column() >= 7 ||
            (role != Qt::DisplayRole && role != Qt::ToolTipRole)) return {};
        const auto row = static_cast<std::size_t>(offset_ + index.row());
        const auto &entry = (*manifest_)[row];
        QString value;
        switch (index.column()) {
        case 0: value = QString::fromUtf8(entry.path); break;
        case 1: value = QString::fromStdString(entry.versionId); break;
        case 2: value = QString::number(static_cast<qulonglong>(entry.size)); break;
        case 3: value = QString::fromStdString(entry.sha256); break;
        case 4: value = gui::snapshotTargetPath(target_, entry.path); break;
        case 5:
            if (checks_.empty()) value = tr("待检查");
            else if (!checks_[row].pathError.isEmpty()) value = checks_[row].pathError;
            else value = rootError_.isEmpty() ? tr("合法") : rootError_;
            break;
        case 6:
            if (checks_.empty()) value = tr("待检查");
            else if (!checks_[row].conflict.isEmpty()) value = checks_[row].conflict;
            else if (target_.isEmpty() || !rootError_.isEmpty() || !checks_[row].pathError.isEmpty())
                value = tr("未检查本地目标");
            else value = tr("未发现");
            break;
        }
        return role == Qt::ToolTipRole ? ui::plainTooltip(value) : value;
    }
    void showManifest(std::shared_ptr<const std::vector<ListedFile>> manifest) {
        beginResetModel();
        manifest_ = std::move(manifest);
        offset_ = 0;
        checks_.clear();
        rootError_.clear();
        endResetModel();
    }
    void setTarget(QString target) {
        target_ = std::move(target);
        checks_.clear();
        rootError_.clear();
        refreshChecks();
    }
    void setChecks(gui::SnapshotPreflightResult result) {
        checks_ = std::move(result.rows);
        rootError_ = std::move(result.rootError);
        refreshChecks();
    }
    bool previous() const { return offset_ > 0; }
    bool next() const {
        return manifest_ && static_cast<std::size_t>(offset_ + DisplayPage) < manifest_->size();
    }
    void movePage(int direction) {
        if ((direction < 0 && !previous()) || (direction > 0 && !next())) return;
        beginResetModel();
        offset_ += direction * DisplayPage;
        endResetModel();
    }
  private:
    void refreshChecks() {
        if (rowCount()) emit dataChanged(index(0, 4), index(rowCount() - 1, 6));
    }
    std::shared_ptr<const std::vector<ListedFile>> manifest_;
    std::vector<gui::SnapshotPathCheck> checks_;
    QString target_, rootError_;
    int offset_ = 0;
};

struct SnapshotCheckJob {
    std::atomic_bool cancelled{false};
    gui::SnapshotPreflightResult result;
    bool failed = false;
};

SnapshotPage::SnapshotPage(QWidget *parent)
    : WorkspacePage(tr("目录快照"), tr("下载服务器当前目录，或恢复已保存的目录快照。"), parent) {
    ui::browserPage(layout_);
    layout_->itemAt(0)->widget()->hide();
    auto *query = new QHBoxLayout;
    root_ = new QLineEdit(this);
    root_->setObjectName("snapshotRoot");
    root_->setPlaceholderText(tr("逻辑目录，例如 docs"));
    root_->setMaxLength(4096);
    refresh_ = new QPushButton(tr("查询快照"), this);
    refresh_->setObjectName("snapshotQuery");
    captureButton_ = new QPushButton(tr("下载文件夹…"), this);
    captureButton_->setObjectName("snapshotDownloadFolder");
    captureButton_->setToolTip(tr("先在服务器创建持久快照，再预览并下载当前已提交的文件。"));
    older_ = new QPushButton(tr("更早快照"), this);
    older_->setObjectName("snapshotOlder");
    cancel_ = new QPushButton(tr("取消查询"), this);
    cancel_->setObjectName("snapshotCancel");
    query->addWidget(root_, 1);
    for (auto *button : {captureButton_, refresh_, older_, cancel_}) query->addWidget(button);
    layout_->addLayout(query);

    snapshots_ = new QTableWidget(0, 4, this);
    snapshots_->setObjectName("snapshotList");
    snapshots_->setHorizontalHeaderLabels({tr("快照 ID"), tr("创建时间（本地）"), tr("文件数"), tr("逻辑根")});
    snapshots_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    snapshots_->setSelectionBehavior(QAbstractItemView::SelectRows);
    snapshots_->setSelectionMode(QAbstractItemView::SingleSelection);
    snapshots_->verticalHeader()->hide();
    snapshots_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout_->addWidget(snapshots_, 1);

    auto *destination = new QHBoxLayout;
    target_ = new QLineEdit(this);
    target_->setObjectName("snapshotTarget");
    target_->setPlaceholderText(tr("选择已有父目录"));
    target_->setReadOnly(true);
    choose_ = new QPushButton(tr("选择父目录"), this);
    choose_->setObjectName("snapshotChooseTarget");
    newName_ = new QLineEdit(this);
    newName_->setObjectName("snapshotNewDirectory");
    newName_->setPlaceholderText(tr("新目录名"));
    newName_->setMaxLength(255);
    restore_ = new QPushButton(tr("恢复快照…"), this);
    restore_->setObjectName("snapshotRestore");
    destination->addWidget(target_, 1);
    destination->addWidget(choose_);
    destination->addWidget(newName_);
    destination->addWidget(restore_);
    layout_->addLayout(destination);
    status_ = new QLabel(this);
    status_->setObjectName("snapshotStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout_->addWidget(status_);

    model_ = new SnapshotPlanModel(this);
    auto *entries = new QTableView(this);
    entries->setObjectName("snapshotEntries");
    entries->setModel(model_);
    entries->setSelectionBehavior(QAbstractItemView::SelectRows);
    entries->setEditTriggers(QAbstractItemView::NoEditTriggers);
    entries->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    entries->setColumnWidth(0, 150);
    entries->setColumnWidth(1, 180);
    entries->setColumnWidth(3, 210);
    entries->setColumnWidth(4, 170);
    entries->verticalHeader()->hide();
    layout_->addWidget(entries, 2);
    auto *navigation = new QHBoxLayout;
    previousEntries_ = new QPushButton(tr("清单上一页"), this);
    nextEntries_ = new QPushButton(tr("清单下一页"), this);
    previousEntries_->setObjectName("snapshotEntriesPrevious");
    nextEntries_->setObjectName("snapshotEntriesNext");
    navigation->addWidget(previousEntries_);
    navigation->addWidget(nextEntries_);
    navigation->addStretch();
    layout_->addLayout(navigation);

    restoreJob_ = new gui::SnapshotRestore(this);
    auto *execution = new QHBoxLayout;
    fileProgress_ = new QProgressBar(this);
    fileProgress_->setObjectName("snapshotFileProgress");
    directoryProgress_ = new QProgressBar(this);
    directoryProgress_->setObjectName("snapshotDirectoryProgress");
    restoreCancel_ = new QPushButton(tr("取消恢复"), this);
    restoreCancel_->setObjectName("snapshotRestoreCancel");
    execution->addWidget(fileProgress_, 1);
    execution->addWidget(directoryProgress_, 1);
    execution->addWidget(restoreCancel_);
    layout_->addLayout(execution);
    restoreStatus_ = new QLabel(this);
    restoreStatus_->setObjectName("snapshotRestoreStatus");
    restoreStatus_->setTextFormat(Qt::PlainText);
    restoreStatus_->setWordWrap(true);
    layout_->addWidget(restoreStatus_);
    for (auto *widget : {static_cast<QWidget *>(fileProgress_), static_cast<QWidget *>(directoryProgress_),
                         static_cast<QWidget *>(restoreCancel_), static_cast<QWidget *>(restoreStatus_)})
        widget->hide();
#ifndef Q_OS_WIN
    restore_->setToolTip(tr("安全目录恢复需要 Windows NTFS。"));
#endif

    connect(root_, &QLineEdit::textChanged, this, [this] {
        if (capture_) return;
        clearList();
        status_->setText(tr("请输入逻辑目录，下载当前文件夹或查询历史快照。"));
    });
    connect(captureButton_, &QPushButton::clicked, this, &SnapshotPage::confirmCapture);
    connect(refresh_, &QPushButton::clicked, this, [this] { queryList({}); });
    connect(root_, &QLineEdit::returnPressed, refresh_, &QPushButton::click);
    connect(older_, &QPushButton::clicked, this, [this] { if (nextBefore_) queryList(nextBefore_); });
    connect(snapshots_, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int previous, int) { if (row != previous) selectSnapshot(row); });
    connect(cancel_, &QPushButton::clicked, this, &SnapshotPage::cancelQuery);
    connect(choose_, &QPushButton::clicked, this, [this] {
        const QPointer<SnapshotPage> guard(this);
        const auto directory = QFileDialog::getExistingDirectory(this, tr("选择恢复目录的父目录"),
            target_->text(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (guard && !directory.isEmpty()) target_->setText(directory);
    });
    connect(target_, &QLineEdit::textChanged, this, [this] { schedulePreflight(); });
    connect(newName_, &QLineEdit::textChanged, this, [this] { schedulePreflight(); });
    connect(restore_, &QPushButton::clicked, this, &SnapshotPage::confirmRestore);
    connect(restoreCancel_, &QPushButton::clicked, restoreJob_, &gui::SnapshotRestore::cancel);
    connect(restoreJob_, &gui::SnapshotRestore::changed, this, &SnapshotPage::showRestoreProgress);
    connect(restoreJob_, &gui::SnapshotRestore::finished, this, [this] {
        schedulePreflight();
        emit restoreFinished(restoreJob_->progress().residue);
    });
    connect(previousEntries_, &QPushButton::clicked, this, [this] { model_->movePage(-1); updateButtons(); });
    connect(nextEntries_, &QPushButton::clicked, this, [this] { model_->movePage(1); updateButtons(); });
    status_->setText(tr("请输入逻辑目录，下载当前文件夹或查询历史快照。"));
    updateButtons();
}

SnapshotPage::~SnapshotPage() {
    disconnect(restoreJob_, nullptr, this, nullptr);
    delete restoreJob_;
    stopRequest();
    if (check_) check_->cancelled = true;
    if (worker_) worker_->wait();
}

bool SnapshotPage::restoring() const { return restoreJob_->busy(); }
void SnapshotPage::cancelRestore() { restoreJob_->cancel(); }
bool SnapshotPage::settlingCapture() const {
    return request_ == Request::Create || request_ == Request::Reconcile;
}
void SnapshotPage::cancelCaptureBeforeSend() {
    if (confirmation_) confirmation_->reject();
    if (request_ == Request::CaptureBaseline) cancelQuery();
}

void SnapshotPage::setEndpoint(const QString &host, quint16 port) {
    if (host == host_ && port == port_) return;
    host_ = host;
    port_ = port;
    // Finish reconciling any submitted mutation against its original endpoint.
    if (settlingCapture()) return;
    capture_.reset();
    clearList();
    status_->setText(tr("服务器已切换，请重新查询快照。"));
}

void SnapshotPage::stopRequest() {
    request_ = Request::None;
    if (!peer_) return;
    auto *peer = peer_;
    peer_ = nullptr;
    disconnect(peer, nullptr, this, nullptr);
    peer->disconnectFromServer();
    peer->deleteLater();
}

void SnapshotPage::clearPlan() {
    currentFolder_ = false;
    preflightReady_ = false;
    ++checkRevision_;
    if (check_) check_->cancelled = true;
    manifest_.reset();
    pending_ = {};
    selected_.reset();
    afterPath_.clear();
    total_ = 0;
    model_->showManifest({});
    setProperty("manifestComplete", false);
}

void SnapshotPage::clearList() {
    stopRequest();
    clearPlan();
    const QSignalBlocker blocker(snapshots_);
    snapshots_->setRowCount(0);
    snapshotRows_.clear();
    nextBefore_.reset();
    updateButtons();
}

void SnapshotPage::fail(const QString &message) {
    if (request_ == Request::Create) {
        capture_->issue = message;
        stopRequest();
        request_ = Request::Reconcile;
        status_->setText(tr("创建结果待确认。正在重新连接并查询已有快照…"));
        connectRequest();
        return;
    }
    const bool uncertain = request_ == Request::Reconcile;
    QString detail = message;
    if (uncertain) {
        detail = tr("%1 · 目录 %2：无法确认本次创建是否提交。%3 %4 请先查询已有快照，避免重复创建。")
            .arg(capture_->host + ':' + QString::number(capture_->port), QString::fromUtf8(rootRequest_),
                 capture_->issue, message);
    } else if (currentFolder_ && selected_) {
        detail = planHeading() + tr(" · 快照已保留。") + message;
    }
    capture_.reset();
    stopRequest();
    clearPlan();
    const QSignalBlocker blocker(snapshots_);
    snapshots_->setCurrentCell(-1, -1);
    status_->setText(detail);
    updateButtons();
    if (uncertain) emit captureSettled(true);
}

void SnapshotPage::cancelQuery() {
    fail(request_ == Request::Create ? tr("用户停止等待创建应答。")
                                    : tr("已取消。请选择快照重新读取。"));
}

void SnapshotPage::confirmCapture() {
    if (!captureButton_->isEnabled()) return;
    auto directory = root_->text();
    if (directory.endsWith('/')) directory.chop(1);
    const auto root = directory.toUtf8().toStdString();
    try { encodeCreateSnapshot(root); }
    catch (const std::exception &) {
        status_->setText(tr("请输入有效的非空服务端逻辑目录。"));
        return;
    }
    const auto revision = checkRevision_;
    auto *confirmation = new QMessageBox(QMessageBox::Question, tr("下载当前文件夹"),
        tr("将为服务器目录 %1 创建一份持久快照，再预览其中已提交的文件。即使取消下载，已创建的快照仍会保留。"
           "如果上次创建结果未确认，请先在快照列表核对。继续创建？").arg(directory),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    confirmation->setAttribute(Qt::WA_DeleteOnClose);
    confirmation->setTextFormat(Qt::PlainText);
    confirmation->setDefaultButton(QMessageBox::Cancel);
    confirmation_ = confirmation;
    updateButtons();
    connect(confirmation, &QMessageBox::finished, this, [this, revision, directory, root](int result) {
        confirmation_.clear();
        updateButtons();
        if (result != QMessageBox::Yes || revision != checkRevision_ || !captureButton_->isEnabled()) return;
        root_->setText(directory);
        clearList();
        rootRequest_ = root;
        capture_ = Capture{host_, port_, 0, {}};
        request_ = Request::CaptureBaseline;
        status_->setText(tr("正在读取目录 %1 的快照基线…").arg(directory));
        connectRequest();
    });
    confirmation->open();
}

void SnapshotPage::queryList(std::optional<std::uint64_t> before) {
    if (capture_) return;
    clearList();
    rootRequest_ = root_->text().toUtf8().toStdString();
    listBefore_ = before;
    try { encodeListSnapshots({rootRequest_, before, ListLimit}); }
    catch (const std::exception &) { fail(tr("请输入有效的服务端逻辑目录。")); return; }
    request_ = Request::List;
    status_->setText(tr("正在查询 %1:%2 的快照…").arg(host_).arg(port_));
    connectRequest();
}

void SnapshotPage::selectSnapshot(int row) {
    if (capture_) return;
    stopRequest();
    clearPlan();
    if (row < 0 || static_cast<std::size_t>(row) >= snapshotRows_.size()) {
        updateButtons();
        return;
    }
    selected_ = snapshotRows_[static_cast<std::size_t>(row)];
    request_ = Request::Info;
    status_->setText(tr("正在核对所选快照…"));
    connectRequest();
}

void SnapshotPage::connectRequest() {
    auto *peer = new QtTcpTransport(this);
    peer_ = peer;
    requestId_ = 1;
    connect(peer, &QtTcpTransport::ready, this, [this, peer] {
        if (peer != peer_) return;
        if (!peer->supportsCapability(DirectorySnapshotCapability)) {
            fail(tr("此服务器不支持目录快照查询。"));
            return;
        }
        sendRequest();
    });
    connect(peer, &QtTcpTransport::failed, this, [this, peer] {
        if (peer == peer_) fail(tr("查询未完成。%1").arg(peer->lastError()));
    });
    connect(peer, &QtTcpTransport::frameReceived, this, [this, peer](const Frame &frame) {
        if (peer == peer_) receive(frame);
    });
    updateButtons();
    Hello offer;
    offer.clientName = "chunkharbor-gui-snapshots";
    offer.capabilities = {"handshake", DirectorySnapshotCapability};
    peer->connectForRequests(capture_ ? capture_->host : host_, capture_ ? capture_->port : port_, std::move(offer));
}

void SnapshotPage::sendRequest() {
    if (!peer_) return;
    auto *peer = peer_;
    Frame frame;
    frame.requestId = ++requestId_;
    std::uint16_t expected = 0;
    try {
        switch (request_) {
        case Request::List:
            frame.type = ListSnapshotsType;
            frame.payload = encodeListSnapshots({rootRequest_, listBefore_, ListLimit});
            expected = SnapshotsPageType;
            break;
        case Request::CaptureBaseline:
        case Request::Reconcile:
            frame.type = ListSnapshotsType;
            frame.payload = encodeListSnapshots({rootRequest_, {}, request_ == Request::CaptureBaseline ? 1U : ListLimit});
            expected = SnapshotsPageType;
            break;
        case Request::Create:
            frame.type = CreateSnapshotType;
            frame.payload = encodeCreateSnapshot(rootRequest_);
            expected = SnapshotInfoType;
            break;
        case Request::Info:
            frame.type = GetSnapshotType;
            frame.payload = encodeGetSnapshot(selected_->id);
            expected = SnapshotInfoType;
            break;
        case Request::Entries:
            frame.type = ListSnapshotEntriesType;
            frame.payload = encodeListSnapshotEntries({selected_->id, afterPath_});
            expected = SnapshotEntriesPageType;
            break;
        case Request::None: return;
        }
        if (!peer->sendRequest(frame, expected) && peer == peer_) fail(tr("无法发送快照请求。"));
    } catch (const std::exception &) { fail(tr("快照查询参数无效。")); }
}

void SnapshotPage::showSnapshots(std::vector<SnapshotInfo> rows) {
    snapshotRows_ = std::move(rows);
    const QSignalBlocker blocker(snapshots_);
    snapshots_->setRowCount(0);
    snapshots_->setRowCount(static_cast<int>(snapshotRows_.size()));
    int row = 0;
    for (const auto &snapshot : snapshotRows_) {
        const QStringList columns{QString::fromStdString(snapshot.id), creationTime(snapshot.createdAtSeconds),
                                  QString::number(snapshot.fileCount), QString::fromUtf8(snapshot.root)};
        for (int col = 0; col < columns.size(); ++col) {
            auto *item = new QTableWidgetItem(columns[col]);
            item->setToolTip(ui::plainTooltip(columns[col]));
            snapshots_->setItem(row, col, item);
        }
        ++row;
    }
}

QString SnapshotPage::planHeading() const {
    return tr("目录 %1 · 快照 %2").arg(QString::fromUtf8(selected_->root), QString::fromStdString(selected_->id));
}

void SnapshotPage::receive(const Frame &frame) {
    try {
        if (frame.type == 15) {
            const auto error = decodeUploadError(frame.payload);
            const auto code = QString::fromStdString(error.code);
            if (request_ == Request::Create && (error.code == "BUSY" || error.code == "QUOTA_EXCEEDED")) {
                stopRequest();
                fail(tr("服务器拒绝创建快照（%1）。").arg(code));
                emit captureSettled(false);
            } else {
                fail(tr("服务器返回快照错误（%1）。").arg(code));
            }
            return;
        }
        if (request_ == Request::List || request_ == Request::CaptureBaseline || request_ == Request::Reconcile) {
            auto page = decodeSnapshotsPage(frame.payload);
            const auto limit = request_ == Request::CaptureBaseline ? 1U : ListLimit;
            if (page.root != rootRequest_ || page.snapshots.size() > limit ||
                (request_ == Request::List && listBefore_ && !page.snapshots.empty() &&
                 page.snapshots.front().sequence >= *listBefore_))
                throw std::runtime_error("snapshot list mismatch");
            std::set<std::string> ids;
            for (const auto &snapshot : page.snapshots)
                if (!ids.insert(snapshot.id).second) throw std::runtime_error("duplicate snapshot");
            if (request_ == Request::CaptureBaseline) {
                if (!page.snapshots.empty()) capture_->baseline = page.snapshots.front().sequence;
                request_ = Request::Create;
                status_->setText(tr("正在创建目录 %1 的持久快照…").arg(QString::fromUtf8(rootRequest_)));
                updateButtons();
                sendRequest();
                return;
            }
            if (request_ == Request::Reconcile) {
                const bool sameEndpoint = capture_->host == host_ && capture_->port == port_;
                const auto endpoint = capture_->host + ':' + QString::number(capture_->port);
                const auto newerEnd = std::find_if(page.snapshots.begin(), page.snapshots.end(),
                    [this](const auto &item) { return item.sequence <= capture_->baseline; });
                page.snapshots.erase(newerEnd, page.snapshots.end());
                const auto count = page.snapshots.size();
                if (sameEndpoint) showSnapshots(std::move(page.snapshots));
                capture_.reset();
                stopRequest();
                status_->setText(tr("%1 · 目录 %2：无法确认本次创建是否提交。")
                    .arg(endpoint, QString::fromUtf8(rootRequest_)) +
                    (sameEndpoint && count ? tr("查询到 %1 个新增候选，请核对后手动选择。").arg(count)
                        : sameEndpoint ? tr("本次查询未发现新增快照。请稍后再次查询。")
                                       : tr("服务器设置已切换，请回到原服务器核对。")) +
                    tr(" 客户端未重试创建。"));
                updateButtons();
                emit captureSettled(true);
                return;
            }
            nextBefore_ = page.nextBefore;
            showSnapshots(std::move(page.snapshots));
            stopRequest();
            status_->setText(snapshotRows_.empty() ? tr("此逻辑目录没有快照。") : tr("请选择快照读取完整清单。"));
            updateButtons();
            return;
        }
        if (request_ == Request::Create) {
            auto info = decodeSnapshotInfo(frame.payload);
            if (info.root != rootRequest_ || info.sequence <= capture_->baseline)
                throw std::runtime_error("created snapshot does not match request");
            const bool sameEndpoint = capture_->host == host_ && capture_->port == port_;
            const auto endpoint = capture_->host + ':' + QString::number(capture_->port);
            capture_.reset();
            request_ = Request::Info;
            if (!sameEndpoint) {
                fail(tr("快照 %1 已保留在 %2。服务器设置已切换，请回到原服务器读取。")
                    .arg(QString::fromStdString(info.id), endpoint));
            } else {
                currentFolder_ = true;
                selected_ = std::move(info);
                showSnapshots({*selected_});
                {
                    const QSignalBlocker blocker(snapshots_);
                    snapshots_->setCurrentCell(0, 0);
                }
                status_->setText(planHeading() + tr(" · 快照已创建，正在核对清单…"));
                updateButtons();
                sendRequest();
            }
            emit captureSettled(false);
            return;
        }
        if (request_ == Request::Info) {
            const auto info = decodeSnapshotInfo(frame.payload);
            if (!selected_ || info.id != selected_->id || info.root != selected_->root ||
                info.sequence != selected_->sequence || info.fileCount != selected_->fileCount ||
                info.createdAtSeconds != selected_->createdAtSeconds)
                throw std::runtime_error("snapshot metadata mismatch");
            pending_.reserve(info.fileCount);
            request_ = Request::Entries;
            sendRequest();
            return;
        }
        if (request_ != Request::Entries || !selected_) throw std::runtime_error("unexpected response");
        auto page = decodeSnapshotEntriesPage(frame.payload);
        if (page.snapshotId != selected_->id ||
            (!page.entries.empty() && page.entries.front().path <= afterPath_) ||
            pending_.size() + page.entries.size() > selected_->fileCount)
            throw std::runtime_error("snapshot entries mismatch");
        for (auto &entry : page.entries) {
            total_ += entry.size;
            pending_.push_back(std::move(entry));
        }
        if (!page.nextAfterPath.empty()) {
            if (pending_.size() >= selected_->fileCount)
                throw std::runtime_error("cursor after last entry");
            afterPath_ = std::move(page.nextAfterPath);
            status_->setText(planHeading() + tr(" · 已读取 %1 / %2 项…").arg(pending_.size()).arg(selected_->fileCount));
            sendRequest();
            return;
        }
        if (pending_.size() != selected_->fileCount) throw std::runtime_error("incomplete manifest");
        stopRequest();
        manifest_ = std::make_shared<const std::vector<ListedFile>>(std::move(pending_));
        model_->showManifest(manifest_);
        setProperty("manifestComplete", true);
        schedulePreflight();
    } catch (const std::exception &) {
        fail(tr("快照响应无效或清单不完整，无法生成预览。"));
    }
}

void SnapshotPage::updateButtons() {
    const auto busy = restoring();
    const bool editable = !busy && !capture_ && !confirmation_;
    older_->setEnabled(!busy && !peer_ && nextBefore_.has_value());
    cancel_->setEnabled(!busy && request_ != Request::Reconcile && (peer_ || (manifest_ && worker_)));
    cancel_->setText(request_ == Request::Create ? tr("停止等待并核对") : tr("取消查询"));
    for (auto *widget : {static_cast<QWidget *>(root_), static_cast<QWidget *>(snapshots_),
                         static_cast<QWidget *>(refresh_), static_cast<QWidget *>(choose_),
                         static_cast<QWidget *>(newName_)})
        widget->setEnabled(editable);
#ifdef Q_OS_WIN
    const bool localEndpoint = host_ == "127.0.0.1" && port_;
    captureButton_->setEnabled(editable && !peer_ && localEndpoint && !root_->text().isEmpty());
    restore_->setEnabled(editable && !peer_ && !worker_ && preflightReady_ && localEndpoint);
    restore_->setToolTip(host_ == "127.0.0.1" && port_ ? QString{} : tr("请通过本机地址或 SSH 隧道恢复。"));
#else
    captureButton_->setEnabled(false);
    restore_->setEnabled(false);
#endif
    restore_->setText(currentFolder_ ? tr("下载到新目录…") : tr("恢复快照…"));
    restoreCancel_->setEnabled(busy);
    previousEntries_->setEnabled(model_->previous());
    nextEntries_->setEnabled(model_->next());
}

void SnapshotPage::schedulePreflight() {
    preflightReady_ = false;
    ++checkRevision_;
    if (check_) check_->cancelled = true;
    model_->setTarget(destination());
    if (manifest_) {
        if (currentFolder_ && manifest_->empty()) {
            status_->setText(planHeading() + tr(" · 没有已提交文件。空快照已保留，未创建本地目录。"));
        } else {
            status_->setText(planHeading() + tr(" · %1 个文件 · %2 字节。正在检查预览…")
                .arg(manifest_->size()).arg(total_));
            startPreflight();
        }
    }
    updateButtons();
}

void SnapshotPage::startPreflight() {
    if (worker_ || !manifest_ || (currentFolder_ && manifest_->empty())) return;
    auto job = std::make_shared<SnapshotCheckJob>();
    check_ = job;
    const auto revision = checkRevision_;
    const auto name = newName_->text();
    auto *worker = QThread::create([job, manifest = manifest_, directory = destination(), name] {
        try {
            if (!name.isEmpty() && (!gui::snapshotSafeComponent(name) ||
                name.startsWith(".chunkharbor-restore-", Qt::CaseInsensitive))) {
                job->result.rows.resize(manifest->size());
                job->result.rootError = tr("新目录名无效或使用保留前缀。");
            } else {
                job->result = gui::preflightSnapshot(directory, *manifest, job->cancelled, !name.isEmpty());
            }
        }
        catch (...) { job->failed = true; }
    });
    worker->setParent(this);
    worker_ = worker;
    connect(worker, &QThread::finished, this, [this, worker, job, revision] {
        worker_ = nullptr;
        check_.reset();
        worker->deleteLater();
        if (revision == checkRevision_ && manifest_ && !job->cancelled) {
            if (job->failed) {
                status_->setText(tr("清单完整，但目标预检失败。"));
            } else {
                const auto rootError = job->result.rootError;
                const auto issueCount = rootError.isEmpty()
                    ? static_cast<std::size_t>(std::count_if(job->result.rows.begin(), job->result.rows.end(),
                        [](const auto &row) { return !row.pathError.isEmpty() || !row.conflict.isEmpty(); }))
                    : manifest_->size();
                preflightReady_ = !target_->text().isEmpty() && !newName_->text().isEmpty() &&
                    rootError.isEmpty() && issueCount == 0;
                model_->setChecks(std::move(job->result));
                const auto summary = planHeading() + tr(" · %1 个文件 · %2 字节 · %3 项需处理")
                    .arg(manifest_->size()).arg(total_).arg(issueCount);
                if (!rootError.isEmpty()) status_->setText(summary + "。" + rootError);
                else if (manifest_->empty()) status_->setText(summary + tr("。这是空快照。"));
                else if (target_->text().isEmpty()) status_->setText(summary + tr("。请选择目标目录。"));
                else if (newName_->text().isEmpty()) status_->setText(summary + tr("。请输入新目录名。"));
                else status_->setText(summary + tr("。只读预览已完成。"));
            }
        } else {
            startPreflight();
        }
        updateButtons();
    });
    worker->start();
    updateButtons();
}

QString SnapshotPage::destination() const {
    if (target_->text().isEmpty() || newName_->text().isEmpty()) return target_->text();
    if (!gui::snapshotSafeComponent(newName_->text())) return {};
    return QDir(target_->text()).filePath(newName_->text());
}

void SnapshotPage::confirmRestore() {
    if (!restore_->isEnabled() || !selected_ || !manifest_) return;
    const auto revision = checkRevision_;
    const auto target = destination();
    auto *confirmation = new QMessageBox(QMessageBox::Question,
        currentFolder_ ? tr("确认下载文件夹") : tr("确认恢复快照"),
        tr("将快照 %1 的 %2 个文件（%3 字节）恢复到新目录 %4？")
            .arg(QString::fromStdString(selected_->id)).arg(manifest_->size()).arg(total_).arg(target),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    confirmation->setAttribute(Qt::WA_DeleteOnClose);
    confirmation->setTextFormat(Qt::PlainText);
    confirmation->setDefaultButton(QMessageBox::Cancel);
    confirmation_ = confirmation;
    updateButtons();
    connect(confirmation, &QMessageBox::finished, this, [this, revision, target](int result) {
        confirmation_.clear();
        updateButtons();
        if (result != QMessageBox::Yes || revision != checkRevision_ || !restore_->isEnabled()) return;
        restoreContext_ = planHeading();
        if (!restoreJob_->start(host_, port_, *selected_, manifest_, target))
            status_->setText(tr("无法启动恢复，请重新查询快照。"));
    });
    confirmation->open();
}

void SnapshotPage::showRestoreProgress() {
    const auto &progress = restoreJob_->progress();
    fileProgress_->setRange(0, static_cast<int>(qMax<quint64>(progress.fileSize, 1)));
    fileProgress_->setValue(progress.fileSize ? static_cast<int>(progress.fileBytes) : int(progress.fileFlushed));
    fileProgress_->setFormat(tr("文件 %1 / %2 字节").arg(progress.fileBytes).arg(progress.fileSize));
    directoryProgress_->setRange(0, static_cast<int>(qMax<std::size_t>(progress.count, 1)));
    directoryProgress_->setValue(progress.count ? static_cast<int>(progress.files) : int(progress.published));
    directoryProgress_->setFormat(tr("目录 %1 / %2 文件").arg(progress.files).arg(progress.count));
    restoreStatus_->setText(tr("%1\n%2\n%3 · 已下载 %4 / %5 字节")
        .arg(restoreContext_, progress.status, progress.file).arg(progress.bytes).arg(progress.size));
    fileProgress_->show();
    directoryProgress_->show();
    restoreCancel_->show();
    restoreStatus_->show();
    updateButtons();
}
