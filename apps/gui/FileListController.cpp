#include "FileListController.h"
#include <QThread>
#include <algorithm>
namespace {
bool before(const std::string &a, const std::string &b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                       [](unsigned char x, unsigned char y) { return x < y; });
}
}
FileListController::FileListController(QObject *parent)
    : QObject(parent), transport_(this), model_(this), status_(tr("尚未查询服务器，请点击刷新列表。")) {
    connect(&transport_, &QtTcpTransport::ready, this, [this] {
        if (!busy_) return;
        if (!transport_.supportsCapability("file_list_v1")) {
            fail(tr("服务器不支持文件列表，请使用包含 M6 列表功能的服务端。"));
            return;
        }
        transport_.sendRequest({11, 2, chunkharbor::protocol::encodeListFiles({pendingCursor_, 32})}, 12);
    });
    connect(&transport_, &QtTcpTransport::failed, this, [this] { fail(transport_.lastError()); });
    connect(&transport_, &QtTcpTransport::frameReceived, this, [this](const chunkharbor::protocol::Frame &frame) {
        if (!busy_) return;
        try {
            if (frame.type == 15) {
                auto error = chunkharbor::protocol::decodeUploadError(frame.payload);
                fail(tr("服务器拒绝列表查询（%1）。").arg(QString::fromStdString(error.code)));
                return;
            }
            auto page = chunkharbor::protocol::decodeFilesResult(frame.payload);
            if (page.files.size() > 32) throw std::runtime_error("page exceeds requested limit");
            for (const auto &f : page.files)
                if (!before(pendingCursor_, f.path)) throw std::runtime_error("cursor did not advance");
            if (action_ == Action::Refresh) history_.clear();
            else if (action_ == Action::Next) {
                history_.push_back(cursor_);
                if (history_.size() > 128) history_.pop_front();
            } else history_.pop_back();
            cursor_ = pendingCursor_;
            // Display ordinal only: the server cursor remains the pagination authority.
            if (action_ == Action::Refresh) pageNumber_ = 1;
            else if (action_ == Action::Next) ++pageNumber_;
            else if (pageNumber_ > 1) --pageNumber_;
            model_.setPage(std::move(page));
            hasPage_ = true;
            busy_ = false;
            transport_.disconnectFromServer();
            status_ = model_.rowCount() ? tr("本页 %1 条；列表为查询时的实时结果，不代表历史快照。").arg(model_.rowCount())
                                      : pendingCursor_.empty() ? tr("服务器当前没有已提交文件。")
                                                               : tr("此游标之后暂无已提交文件，可返回上一页或刷新。");
            emit changed();
            emit finished(true);
        } catch (const std::exception &) { fail(tr("服务器文件列表无效：字段、顺序或分页游标不符合协议。")); }
    });
}
void FileListController::setEndpoint(const QString &host, quint16 port) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (host == host_ && port == port_) return;
    transport_.disconnectFromServer();
    busy_ = false;
    host_ = host;
    port_ = port;
    history_.clear();
    cursor_.clear();
    pendingCursor_.clear();
    model_.setPage({});
    hasPage_ = false;
    pageNumber_ = 1;
    status_ = tr("连接设置已更改，请刷新列表。旧服务器结果已清空。");
    emit changed();
}
void FileListController::refresh() { start(Action::Refresh, ""); }
void FileListController::next() { if (hasNext()) start(Action::Next, model_.page().nextAfterPath); }
void FileListController::previous() { if (hasPrevious()) start(Action::Previous, history_.back()); }
void FileListController::start(Action action, std::string cursor) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (busy_) return;
    if (host_ != "127.0.0.1" || !port_) { fail(tr("文件列表仅支持本地地址或 SSH 隧道。")); return; }
    action_ = action;
    pendingCursor_ = std::move(cursor);
    busy_ = true;
    status_ = hasPage_ ? tr("正在查询…当前仍显示上次结果。") : tr("正在查询服务器文件…");
    emit changed();
    chunkharbor::protocol::Hello offer;
    offer.clientName = "chunkharbor-gui-files";
    offer.capabilities = {"handshake", "file_list_v1"};
    transport_.connectForRequests(host_, port_, std::move(offer));
}
void FileListController::cancel() {
    if (!busy_) return;
    fail(tr("查询已取消。"));
}
void FileListController::fail(const QString &message) {
    const bool active = busy_;
    busy_ = false;
    transport_.disconnectFromServer();
    status_ = message + (hasPage_ ? tr(" 当前保留上次结果，请刷新后重试。") : QString{});
    emit changed();
    if (active) emit finished(false);
}
