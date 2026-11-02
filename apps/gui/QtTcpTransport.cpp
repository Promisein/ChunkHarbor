#include "QtTcpTransport.h"
#include <QThread>
#include <exception>

namespace {
constexpr qint64 ReceiveLimit =
    chunkharbor::protocol::HeaderSize + chunkharbor::protocol::MaxHandshakePayload;
}
QtTcpTransport::QtTcpTransport(QObject *parent) : QObject(parent), socket_(this), timer_(this) {
    socket_.setReadBufferSize(ReceiveLimit + 1);
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] { fail(tr("连接或握手超时（5 秒）。")); });
    connect(&socket_, &QTcpSocket::connected, this, [this] {
        if (terminal_)
            return;
        emit statusChanged(tr("已连接，正在检查协议…"));
        try {
            const auto bytes = chunkharbor::protocol::encode(exchange_->request());
            if (socket_.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
                static_cast<qint64>(bytes.size()))
                fail(tr("无法发送握手请求。"));
        } catch (const std::exception &) {
            fail(tr("无法创建握手请求。"));
        }
    });
    connect(&socket_, &QTcpSocket::readyRead, this, &QtTcpTransport::receive);
    connect(&socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        fail(tr("连接失败：%1").arg(socket_.errorString()));
    });
    connect(&socket_, &QTcpSocket::disconnected, this, [this] {
        if (exchange_ && exchange_->ready())
            emit statusChanged(tr("连接已断开，请重新连接以检查服务器。"));
        else
            fail(tr("服务器在握手完成前断开连接。"));
    });
}
void QtTcpTransport::connectToServer(const QString &host, quint16 port) {
    Q_ASSERT(QThread::currentThread() == thread());
    terminal_ = true;
    socket_.abort();
    received_.clear();
    chunkharbor::protocol::Hello offer;
    offer.clientName = "chunkharbor-gui";
    exchange_.emplace(std::move(offer));
    terminal_ = false;
    emit statusChanged(tr("正在连接…"));
    timer_.start(5000);
    socket_.connectToHost(host, port);
}
void QtTcpTransport::disconnectFromServer() {
    Q_ASSERT(QThread::currentThread() == thread());
    terminal_ = true;
    timer_.stop();
    exchange_.reset();
    socket_.abort();
    received_.clear();
    emit statusChanged(tr("连接设置已更改，请重新检查连接。"));
}
void QtTcpTransport::fail(const QString &message) {
    if (terminal_)
        return;
    terminal_ = true;
    timer_.stop();
    socket_.abort();
    emit statusChanged(message);
    emit failed();
}
void QtTcpTransport::receive() {
    if (terminal_) {
        socket_.abort();
        return;
    }
    const auto available = socket_.bytesAvailable();
    if (available > ReceiveLimit - received_.size()) {
        fail(tr("服务器握手消息超过大小限制。"));
        return;
    }
    received_ += socket_.read(available);
    try {
        // Reject advertised oversized handshake lengths before waiting for a body.
        if (received_.size() >= 32) {
            quint64 length = 0;
            for (int i = 20; i < 28; ++i)
                length = (length << 8) | static_cast<unsigned char>(received_[i]);
            if (length > chunkharbor::protocol::MaxHandshakePayload) {
                fail(tr("服务器握手消息超过大小限制。"));
                return;
            }
        }
        const auto result = chunkharbor::protocol::decode(
            {received_.constData(), static_cast<std::size_t>(received_.size())});
        if (result.status == chunkharbor::protocol::DecodeStatus::Error) {
            fail(tr("协议帧无效。"));
            return;
        }
        if (result.status == chunkharbor::protocol::DecodeStatus::NeedMore)
            return;
        if (result.consumed != static_cast<std::size_t>(received_.size())) {
            fail(tr("握手消息后出现了意外数据。"));
            return;
        }
        exchange_->accept(result.frame);
        terminal_ = true;
        timer_.stop();
        received_.clear();
        emit statusChanged(tr("协议握手成功（版本 1），当前仅验证连接。"));
        emit ready();
    } catch (const std::exception &) {
        fail(tr("服务器握手被拒绝：响应无效或协议不兼容。"));
    }
}
