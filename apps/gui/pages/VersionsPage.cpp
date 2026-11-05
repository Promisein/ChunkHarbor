#include "VersionsPage.h"
#include "WorkspaceStore.h"
#include "QtTcpTransport.h"
#include "DownloadController.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QPointer>
#include "chunkharbor/protocol/VersionHistory.h"
#include <QHeaderView>
#include <QTableWidget>
#include "widgets/BrowserUi.h"
#include "widgets/TableKit.h"
#include <QDateTime>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QRegularExpression>
#include <QPainter>
#include <QSet>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStyle>
#include <algorithm>

namespace {
constexpr int RecordRole = Qt::UserRole + 2;
QDateTime instant(const QString &raw) {
    // Old/local records without an offset cannot safely be called UTC or local.
    static const QRegularExpression zone("(?:Z|[+-][0-9]{2}:?[0-9]{2})$");
    return zone.match(raw).hasMatch() ? QDateTime::fromString(raw, Qt::ISODateWithMs) : QDateTime{};
}
QString localTime(const QString &raw) {
    const auto time = instant(raw);
    return time.isValid() ? time.toLocalTime().toString("yyyy-MM-dd HH:mm")
                          : QObject::tr("时间或时区未知");
}
QString shortId(const QString &id) { return id.size() > 12 ? id.left(12) + "…" : id; }
class VersionDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem styled(option);
        initStyleOption(&styled, index);
        const auto record = index.data(RecordRole).value<gui::FileRecord>();
        styled.text.clear();
        const auto *widget = option.widget;
        (widget ? widget->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &styled, painter, widget);
        painter->save();
        const bool selected = styled.state.testFlag(QStyle::State_Selected);
        auto rect = option.rect.adjusted(12, 10, -12, -10);
        const QStringList lines{localTime(record.time), record.change + " · " + gui::formatBytes(record.size), shortId(record.versionId)};
        for (int line = 0; line < lines.size(); ++line) {
            auto font = option.font;
            font.setBold(line == 0);
            painter->setFont(font);
            painter->setPen(styled.palette.color(!selected && line == 2 ? QPalette::PlaceholderText : QPalette::Text));
            const QFontMetrics metrics(font);
            painter->drawText(QRect(rect.x(), rect.y() + line * option.fontMetrics.lineSpacing(), rect.width(), option.fontMetrics.lineSpacing()),
                              Qt::AlignLeft | Qt::AlignVCenter, metrics.elidedText(lines[line], Qt::ElideRight, rect.width()));
        }
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        return {0, option.fontMetrics.lineSpacing() * 3 + 24};
    }
};
}

// Let small pages end at the last row, while a full page can shrink and scroll.
class ServerHistoryRows final : public QTableWidget {
  public:
    using QTableWidget::QTableWidget;
    QSize sizeHint() const override {
        return {QTableWidget::sizeHint().width(),
                horizontalHeader()->sizeHint().height() + verticalHeader()->length() + 2 * frameWidth()};
    }
};

class ServerHistoryPanel final : public QWidget {
  public:
    explicit ServerHistoryPanel(QWidget *parent, DownloadController *download, QPushButton *source)
        : QWidget(parent), download_(download) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
        auto *controls = new QHBoxLayout;
        auto *back = new QPushButton(tr("返回本地提交记录"), this);
        back->setObjectName("serverHistoryBack");
        controls->addWidget(back);
        connect(back, &QPushButton::clicked, source, [source] { source->setChecked(false); });
        path_ = new QLineEdit(this);
        path_->setObjectName("serverHistoryPath");
        path_->setMaxLength(4096);
        path_->setMinimumWidth(120);
        path_->setPlaceholderText(tr("逻辑路径，例如 docs/readme.txt"));
        controls->addWidget(path_, 1);
        auto *first = new QPushButton(tr("查询最新"), this);
        first->setObjectName("serverHistoryFirst");
        first->setProperty("primary", true);
        next_ = new QPushButton(tr("更早版本"), this);
        next_->setObjectName("serverHistoryNext");
        cancel_ = new QPushButton(tr("取消查询"), this);
        cancel_->setObjectName("serverHistoryCancel");
        for (auto *button : {first, next_, cancel_}) controls->addWidget(button);
        layout->addLayout(controls);
        status_ = new QLabel(this);
        status_->setObjectName("serverHistoryStatus");
        status_->setWordWrap(true);
        status_->setTextFormat(Qt::PlainText);
        layout->addWidget(status_);
        auto *splitter = new QSplitter(Qt::Horizontal, this);
        splitter->setObjectName("serverHistorySplitter");
        ui::browserSplitter(splitter);
        auto *list = new QWidget(splitter);
        list->setMinimumWidth(360);
        auto *listLayout = new QVBoxLayout(list);
        listLayout->setContentsMargins(0, 0, 0, 0);
        listLayout->setSpacing(10);
        auto *title = new QLabel(tr("服务器历史版本"), list);
        title->setProperty("role", "detailPanelTitle");
        listLayout->addWidget(title);
        file_ = new ui::ElidedLabel(list);
        file_->setProperty("role", "muted");
        listLayout->addWidget(file_);

        rows_ = new ServerHistoryRows(0, 7, list);
        rows_->setObjectName("serverHistoryRows");
        rows_->setHorizontalHeaderLabels({tr("序号"), tr("版本 ID"), tr("大小"),
            tr("SHA-256"), tr("提交时间（本地）"), tr("状态"), tr("SHA-256 前缀")});
        ui::browserList(rows_);
        rows_->setSelectionBehavior(QAbstractItemView::SelectRows);
        rows_->setShowGrid(false);
        rows_->setWordWrap(false);
        rows_->verticalHeader()->hide();
        rows_->verticalHeader()->setDefaultSectionSize(ui::BrowserRowHeight);
        rows_->hideColumn(1);
        rows_->hideColumn(3);
        auto *header = rows_->horizontalHeader();
        header->moveSection(header->visualIndex(5), 1);
        header->moveSection(header->visualIndex(4), 2);
        header->setSectionResizeMode(QHeaderView::Stretch);
        header->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        rows_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        listLayout->addWidget(rows_);
        empty_ = new QLabel(list);
        empty_->setObjectName("serverHistoryEmpty");
        empty_->setProperty("role", "browserEmptyState");
        empty_->setWordWrap(true);
        empty_->setTextFormat(Qt::PlainText);
        listLayout->addWidget(empty_);
        listLayout->addStretch(1);

        auto *right = new QWidget(splitter);
        auto *rightLayout = new QVBoxLayout(right);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        auto *details = new InfoCard(tr("版本详情"), right);
        details->setObjectName("serverHistoryDetails");
        ui::detailPanel(details);
        right->setMinimumWidth(details->minimumWidth());
        right->setMaximumWidth(details->maximumWidth());
        id_ = ui::detailField(details, tr("版本 ID"), "serverHistoryVersionId", true);
        state_ = ui::detailField(details, tr("版本状态"), "serverHistoryVersionState");
        time_ = ui::detailField(details, tr("提交时间（本地）"), "serverHistoryVersionTime");
        size_ = ui::detailField(details, tr("文件大小"), "serverHistoryVersionSize");
        sha_ = ui::detailField(details, tr("内容 SHA-256"), "serverHistoryVersionSha", true);
        idCopy_ = details->findChild<QPushButton *>("serverHistoryVersionIdCopy");
        shaCopy_ = details->findChild<QPushButton *>("serverHistoryVersionShaCopy");
        restore_ = new QPushButton(tr("下载此版本"), details);
        restore_->setObjectName("serverHistoryRestore");
        restore_->setProperty("primary", true);
        details->body()->addWidget(restore_);
        rightLayout->addWidget(details);
        rightLayout->addStretch(1);
        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 0);
        splitter->setSizes({650, 350});
        layout->addWidget(splitter, 1);
        connect(rows_, &QTableWidget::currentCellChanged, this, [this] {
            updateDetails();
            updateRestore();
        });
        connect(restore_, &QPushButton::clicked, this, [this] { restoreSelected(); });
        if (download_)
            connect(download_, &DownloadController::changed, this, [this] { updateRestore(); });
        connect(path_, &QLineEdit::textChanged, this, [this] { reset(); });
        connect(path_, &QLineEdit::returnPressed, this, [this] { request({}); });
        connect(first, &QPushButton::clicked, this, [this] { request({}); });
        connect(next_, &QPushButton::clicked, this, [this] { if (nextBefore_) request(nextBefore_); });
        connect(cancel_, &QPushButton::clicked, this, [this] { finish(tr("查询已取消。")); });
        reset();
    }

    ~ServerHistoryPanel() override { stop(); }

    void setEndpoint(const QString &host, quint16 port) {
        if (host_ == host && port_ == port) return;
        host_ = host;
        port_ = port;
        reset();
    }
    void usePath(const QString &path) { path_->setText(path); }
    void leave() { if (peer_) finish(tr("查询已取消。")); }

  private:
    void updateDetails() {
        const int row = rows_->currentRow();
        const bool selected = row >= 0 && static_cast<std::size_t>(row) < page_.versions.size();
        idCopy_->setEnabled(selected);
        shaCopy_->setEnabled(selected);
        id_->setValue(selected ? rows_->item(row, 1)->text() : tr("选择版本查看详情"));
        state_->setValue(selected ? rows_->item(row, 5)->text() : QString{});
        time_->setValue(selected ? rows_->item(row, 4)->text() : QString{});
        size_->setValue(selected ? rows_->item(row, 2)->text() : QString{});
        sha_->setValue(selected ? rows_->item(row, 3)->text() : QString{});
    }
    void updateListPresentation() {
        const bool hasRows = rows_->rowCount() > 0;
        rows_->setVisible(hasRows);
        rows_->updateGeometry();
        empty_->setVisible(!hasRows);
        empty_->setText(tr("暂无版本可显示"));
        file_->setValue(page_.path.empty() ? path_->text() : QString::fromUtf8(page_.path));
        updateDetails();
    }
    bool canRestore() const {
        return download_ && !download_->busy() && !peer_ && rows_->currentRow() >= 0 &&
               static_cast<std::size_t>(rows_->currentRow()) < page_.versions.size();
    }
    void updateRestore() { restore_->setEnabled(canRestore()); }
    void restoreSelected() {
        if (!canRestore()) return;
        const auto &row = page_.versions[static_cast<std::size_t>(rows_->currentRow())];
        const chunkharbor::protocol::ListedFile file{page_.path, row.versionId, row.size, row.sha256};
        const auto host = host_;
        const auto port = port_;
        const QPointer<ServerHistoryPanel> guard(this);
        const auto target = QFileDialog::getSaveFileName(
            this, tr("下载版本 %1（不覆盖已有文件）").arg(QString::fromStdString(file.versionId)),
            QFileInfo(QString::fromUtf8(file.path)).fileName(), {}, nullptr, QFileDialog::DontConfirmOverwrite);
        if (!guard || target.isEmpty()) return;
        if (!download_->start(host, port, file, target, chunkharbor::protocol::DownloadSelection::Historical))
            status_->setText(tr("无法启动历史版本下载，请检查连接或等待当前下载结束。"));
    }
    void stop() {
        if (!peer_) return;
        auto *peer = peer_;
        peer_ = nullptr;
        disconnect(peer, nullptr, this, nullptr);
        peer->disconnectFromServer();
        peer->deleteLater();
    }
    void finish(const QString &message) {
        stop();
        status_->setText(message);
        cancel_->setEnabled(false);
        next_->setEnabled(nextBefore_.has_value());
        updateListPresentation();
        updateRestore();
    }
    void reset() {
        nextBefore_.reset();
        page_ = {};
        rows_->setRowCount(0);
        finish(tr("服务器 %1:%2。输入逻辑路径后查询。").arg(host_).arg(port_));
    }
    void request(std::optional<std::uint64_t> before) {
        using namespace chunkharbor::protocol;
        stop();
        const auto path = path_->text().toUtf8().toStdString();
        std::string payload;
        try { payload = encodeListVersions({path, before}); }
        catch (const std::exception &) { finish(tr("请输入有效的文件逻辑路径。")); return; }
        if (!before) {
            page_ = {};
            rows_->setRowCount(0);
            nextBefore_.reset();
        }
        next_->setEnabled(false);
        cancel_->setEnabled(true);
        status_->setText(tr("正在查询 %1:%2…").arg(host_).arg(port_));
        auto *peer = new QtTcpTransport(this);
        peer_ = peer;
        updateListPresentation();
        updateRestore();
        connect(peer, &QtTcpTransport::ready, this, [this, peer, payload] {
            if (peer_ != peer) return;
            if (!peer->supportsCapability(VersionHistoryCapability)) {
                finish(tr("此服务器不支持历史版本查询。"));
                return;
            }
            if (!peer->sendRequest({ListVersionsType, 2, payload}, VersionsResultType) && peer_ == peer)
                finish(tr("无法发送历史查询。"));
        });
        connect(peer, &QtTcpTransport::failed, this, [this, peer] {
            if (peer_ == peer) finish(peer->lastError());
        });
        connect(peer, &QtTcpTransport::frameReceived, this, [this, peer, path, before](const Frame &frame) {
            if (peer_ != peer) return;
            try {
                if (frame.type == 15) {
                    finish(tr("服务器拒绝历史查询（%1）。").arg(QString::fromStdString(decodeUploadError(frame.payload).code)));
                    return;
                }
                auto result = decodeVersionsResult(frame.payload);
                if (result.path != path || result.versions.size() > 32 ||
                    (before && !result.versions.empty() && result.versions.front().sequence >= *before))
                    throw std::runtime_error("history response does not match request");
                showPage(std::move(result));
            } catch (const std::exception &) {
                finish(tr("服务器历史响应无效。"));
            }
        });
        Hello offer;
        offer.clientName = "chunkharbor-gui-history";
        offer.capabilities = {"handshake", VersionHistoryCapability};
        peer->connectForRequests(host_, port_, std::move(offer));
    }

    void showPage(chunkharbor::protocol::VersionsResult result) {
        page_ = std::move(result);
        const QSignalBlocker blocker(rows_);
        rows_->setCurrentCell(-1, -1);
        rows_->setRowCount(static_cast<int>(page_.versions.size()));
        int index = 0;
        for (const auto &version : page_.versions) {
            const auto time = version.committedAtSeconds && *version.committedAtSeconds <= 253402300799ULL
                ? QDateTime::fromSecsSinceEpoch(static_cast<qint64>(*version.committedAtSeconds)).toString("yyyy-MM-dd HH:mm:ss")
                : tr("未知");
            const QStringList cells{QString::number(version.sequence), QString::fromStdString(version.versionId),
                gui::formatBytes(version.size), QString::fromStdString(version.sha256), time,
                version.current ? tr("当前") : tr("历史"),
                QString::fromStdString(version.sha256.substr(0, 8)) + "…"};
            for (int col = 0; col < cells.size(); ++col) {
                auto *item = new QTableWidgetItem(cells[col]);
                item->setToolTip(ui::plainTooltip(col == 6 ? cells[3] : cells[col]));
                rows_->setItem(index, col, item);
            }
            ++index;
        }
        nextBefore_ = page_.nextBefore;
        finish(page_.versions.empty() ? tr("此路径没有服务器历史版本。")
                                       : tr("本页 %1 条，当前标记以本次查询为准。").arg(index));
    }

    QLineEdit *path_;
    QPushButton *next_, *cancel_, *restore_;
    QLabel *status_, *empty_;
    ui::ElidedLabel *file_, *id_, *state_, *time_, *size_, *sha_;
    QPushButton *idCopy_, *shaCopy_;
    QTableWidget *rows_;
    DownloadController *download_;
    chunkharbor::protocol::VersionsResult page_;
    QtTcpTransport *peer_ = nullptr;
    QString host_ = "127.0.0.1";
    quint16 port_ = 18787;
    std::optional<std::uint64_t> nextBefore_;
};

void VersionsPage::setServerEndpoint(const QString &host, quint16 port) {
    serverHistory_->setEndpoint(host, port);
}

VersionsPage::VersionsPage(WorkspaceStore *store, QWidget *parent, DownloadController *download)
    : WorkspacePage(tr("版本历史"), tr("本机提交记录与服务器历史分开查看。"), parent) {
    ui::browserPage(layout_);
    // The main window already names the page. Keep its scope note, not a second large title.
    layout_->itemAt(0)->widget()->hide();
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName("versionsSplitter");
    ui::browserSplitter(splitter);
    auto *files = new QWidget(splitter);
    files->setMinimumWidth(180);
    auto *fileLayout = new QVBoxLayout(files);
    fileLayout->setContentsMargins(0, 0, 0, 0);
    fileLayout->setSpacing(12);
    auto *fileTitle = new QLabel(tr("备份文件"), files);
    fileTitle->setProperty("role", "detailPanelTitle");
    fileLayout->addWidget(fileTitle);
    auto *search = new QLineEdit(files);
    search->setObjectName("versionFileSearch");
    search->setPlaceholderText(tr("筛选本机记录的文件…"));
    search->setToolTip(tr("仅筛选本机已保存的提交记录，不查询服务器。"));
    fileLayout->addWidget(search);
    auto *list = new QListWidget(files);
    list->setObjectName("versionFiles");
    ui::browserList(list);
    list->setWordWrap(false);
    list->setUniformItemSizes(true);
    list->setSpacing(0);
    auto *filesEmpty = ui::browserEmptyState(list, tr("尚无本机提交记录"));
    fileLayout->addWidget(list, 1);

    auto *history = new QWidget(splitter);
    history->setMinimumWidth(200);
    auto *historyLayout = new QVBoxLayout(history);
    historyLayout->setContentsMargins(0, 0, 0, 0);
    historyLayout->setSpacing(12);
    auto *historyHeading = new QHBoxLayout;
    auto *historyTitle = new QLabel(tr("历史版本"), history);
    historyTitle->setProperty("role", "detailPanelTitle");
    historyHeading->addWidget(historyTitle, 1);
    auto *toggle = new QPushButton(tr("详情"), history);
    toggle->setObjectName("versionsToggleDetails");
    toggle->setCheckable(true);
    toggle->setChecked(true);
    toggle->setToolTip(tr("显示或收起版本详情"));
    historyHeading->addWidget(toggle);
    historyLayout->addLayout(historyHeading);
    auto *model = new QStandardItemModel(this);
    model->setObjectName("versionHistoryModel");
    model->setHorizontalHeaderLabels({tr("版本标识"), tr("提交时间"), tr("变化类型"), tr("大小"), tr("变化块数")});
    auto *versions = new QListView(history);
    versions->setObjectName("versionHistory");
    ui::browserList(versions);
    versions->setModel(model);
    versions->setItemDelegate(new VersionDelegate(versions));
    versions->setResizeMode(QListView::Adjust);
    versions->setUniformItemSizes(true);
    auto *historyEmpty = ui::browserEmptyState(versions, tr("选择文件查看本机版本"));
    historyLayout->addWidget(versions, 1);

    auto *details = new InfoCard(tr("版本详情"), splitter);
    details->setObjectName("versionDetailsPanel");
    ui::detailPanel(details);
    auto *id = ui::detailField(details, tr("版本标识"), "versionId", true);
    auto *time = ui::detailField(details, tr("提交时间（本地）"), "versionTime");
    auto *change = ui::detailField(details, tr("变化类型"), "versionChange");
    auto *size = ui::detailField(details, tr("文件大小"), "versionSize");
    auto *base = ui::detailField(details, tr("基线版本"), "versionBase", true);
    auto *blocks = ui::detailField(details, tr("本轮实际变化"), "versionComparison");
    auto *sha = ui::detailField(details, tr("内容 SHA-256"), "versionSha", true);
    auto *note = new QLabel(tr("变化块来自提交时的上传计划，\n并非重新比较历史版本。"), details);
    note->setTextFormat(Qt::PlainText);
    note->setWordWrap(true);
    note->setProperty("role", "muted");
    details->body()->addWidget(note);
    // Fields consume their natural height; spare space stays below the content.
    details->body()->addStretch(1);
    auto *restoreNote = new QLabel(tr("下载指定版本请使用服务器历史。"), history);
    restoreNote->setObjectName("versionRestoreNotice");
    restoreNote->setProperty("role", "muted");
    restoreNote->setWordWrap(true);
    historyLayout->addWidget(restoreNote);
    connect(toggle, &QPushButton::toggled, details, &QWidget::setVisible);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    splitter->setSizes({260, 380, 320});
    auto *source = new QPushButton(tr("查看服务器历史"), this);
    source->setCheckable(true);
    source->setObjectName("versionSource");
    layout_->addWidget(source, 0, Qt::AlignLeft);
    layout_->addWidget(splitter, 1);
    serverHistory_ = new ServerHistoryPanel(this, download, source);
    serverHistory_->hide();
    layout_->addWidget(serverHistory_, 1);
    connect(source, &QPushButton::toggled, this, [=](bool remote) {
        source->setVisible(!remote);
        splitter->setVisible(!remote);
        serverHistory_->setVisible(remote);
        if (remote && list->currentItem()) serverHistory_->usePath(list->currentItem()->text());
        if (!remote) serverHistory_->leave();
    });

    auto updateDetails = [=](const QModelIndex &index) {
        const bool valid = index.isValid();
        for (auto *button : details->findChildren<QPushButton *>()) button->setEnabled(valid);
        if (!valid) {
            for (auto *field : {id, time, change, size, base, blocks, sha}) field->setValue({});
            id->setValue(tr("选择版本查看详情"));
            return;
        }
        const auto record = model->index(index.row(), 0).data(RecordRole).value<gui::FileRecord>();
        id->setValue(record.versionId);
        time->setValue(localTime(record.time));
        time->setToolTip(ui::plainTooltip(tr("原始时间：%1").arg(record.time)));
        change->setValue(record.change);
        size->setValue(gui::formatBytes(record.size));
        size->setToolTip(tr("%1 字节").arg(record.size));
        base->setValue(record.baseVersion.isEmpty() || record.baseVersion == "absent" ? tr("无（首次提交）") : record.baseVersion);
        details->findChild<QPushButton *>("versionBaseCopy")->setEnabled(!record.baseVersion.isEmpty() && record.baseVersion != "absent");
        blocks->setValue(tr("%1 个块 · %2").arg(record.changedBlocks).arg(gui::formatBytes(record.missingBytes)));
        blocks->setToolTip(tr("%1 个块，%2 字节；来自提交时的上传计划。")
                           .arg(record.changedBlocks).arg(record.missingBytes));
        sha->setValue(record.sha256);
    };
    connect(versions->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [=](const QModelIndex &index, const QModelIndex &) { updateDetails(index); });
    auto populate = [=] {
        model->removeRows(0, model->rowCount());
        updateDetails({});
        if (list->currentItem() && !list->currentItem()->isHidden()) {
            const auto key = list->currentItem()->data(Qt::UserRole).toString();
            auto records = store->records();
            std::stable_sort(records.begin(), records.end(), [](const auto &a, const auto &b) {
                const auto at = instant(a.time), bt = instant(b.time);
                if (at.isValid() && bt.isValid()) return at > bt;
                if (at.isValid() != bt.isValid()) return at.isValid();
                return a.time > b.time;
            });
            for (const auto &record : records) {
                if (record.endpoint + "/" + record.logicalPath != key) continue;
                ui::row(model, {record.versionId, record.time, record.change, gui::formatBytes(record.size),
                                QString::number(record.changedBlocks)});
                auto *item = model->item(model->rowCount() - 1, 0);
                item->setData(record.sha256, Qt::UserRole);
                item->setData(QVariant::fromValue(record), RecordRole);
                item->setToolTip(ui::plainTooltip(tr("版本：%1\n提交时间：%2\n%3 · %4")
                    .arg(record.versionId, record.time, record.change, gui::formatBytes(record.size))));
            }
        }
        historyEmpty->setVisible(model->rowCount() == 0);
        if (model->rowCount()) versions->setCurrentIndex(model->index(0, 0));
    };
    auto filter = [=] {
        QListWidgetItem *first = nullptr;
        for (int i = 0; i < list->count(); ++i) {
            auto *item = list->item(i);
            item->setHidden(!item->text().contains(search->text(), Qt::CaseInsensitive));
            if (!item->isHidden() && !first) first = item;
        }
        if (!list->currentItem() || list->currentItem()->isHidden()) list->setCurrentItem(first);
        filesEmpty->setText(store->records().isEmpty() ? tr("尚无本机提交记录") : tr("没有匹配的本机文件"));
        filesEmpty->setVisible(!first);
        populate();
    };
    auto refresh = [=] {
        const auto previous = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString{};
        {
            QSignalBlocker blocker(list);
            list->clear();
            QSet<QString> keys;
            for (const auto &record : store->records()) {
                const auto key = record.endpoint + "/" + record.logicalPath;
                if (keys.contains(key)) continue;
                keys.insert(key);
                auto *item = new QListWidgetItem(record.logicalPath, list);
                item->setSizeHint(QSize(0, ui::BrowserRowHeight));
                item->setToolTip(ui::plainTooltip(record.logicalPath + "\n" + record.endpoint));
                item->setData(Qt::UserRole, key);
                if (key == previous) list->setCurrentItem(item);
            }
            if (list->currentRow() < 0 && list->count()) list->setCurrentRow(0);
        }
        filter();
    };
    connect(list, &QListWidget::currentRowChanged, this, [=](int) { populate(); });
    connect(search, &QLineEdit::textChanged, this, [=](const QString &) { filter(); });
    connect(store, &WorkspaceStore::changed, this, refresh);
    refresh();
}
