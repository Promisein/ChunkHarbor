#include "QtTcpTransport.h"
#include <QThread>
#include <algorithm>
#include <exception>
namespace {
using namespace chunkharbor::protocol;
constexpr qint64 HandshakeLimit = HeaderSize + MaxHandshakePayload;
constexpr qint64 ControlLimit = HeaderSize + MaxControlPayload;
}
QtTcpTransport::QtTcpTransport(QObject *parent) : QObject(parent), socket_(this), timer_(this) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] {
        fail(handshaken_ ? tr("服务器响应超时（10 秒）。") : tr("连接或握手超时（5 秒）。"));
    });
    connect(&socket_, &QTcpSocket::connected, this, [this] {
        if (terminal_) return;
        emit statusChanged(tr("已连接，正在检查协议…"));
        try {
            const auto bytes = encode(exchange_->request());
            if (socket_.write(bytes.data(), static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size()))
                fail(tr("无法发送握手请求。"));
        } catch (const std::exception &) { fail(tr("无法创建握手请求。")); }
    });
    connect(&socket_, &QTcpSocket::readyRead, this, &QtTcpTransport::receive);
    connect(&socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        fail(tr("连接失败：%1").arg(socket_.errorString()));
    });
    connect(&socket_, &QTcpSocket::disconnected, this, [this] {
        if (terminal_) {
            if (handshaken_ && !requestMode_) emit statusChanged(tr("连接已断开，请重新连接以检查服务器。"));
            return;
        }
        fail(tr("服务器在响应完成前断开连接。"));
    });
}
void QtTcpTransport::connectToServer(const QString &host, quint16 port) {
    Hello offer;
    offer.clientName = "chunkharbor-gui";
    start(host, port, std::move(offer), false);
}
void QtTcpTransport::connectForRequests(const QString &host, quint16 port, Hello offer) {
    start(host, port, std::move(offer), true);
}
void QtTcpTransport::start(const QString &host, quint16 port, Hello offer, bool requestMode) {
    Q_ASSERT(QThread::currentThread() == thread());
    terminal_ = true;
    handshaken_ = false;
    socket_.abort();
    timer_.stop();
    received_.clear();
    lastError_.clear();
    requestMode_ = requestMode;
    awaiting_ = false;
    lastRequestId_ = 1;
    socket_.setReadBufferSize(HandshakeLimit + 1);
    exchange_.emplace(std::move(offer));
    terminal_ = false;
    emit statusChanged(tr("正在连接…"));
    timer_.start(5000);
    socket_.connectToHost(host, port);
}
bool QtTcpTransport::supportsCapability(const std::string &capability) const {
    if (!handshaken_ || !exchange_) return false;
    const auto &caps = exchange_->ack().capabilities;
    return std::find(caps.begin(), caps.end(), capability) != caps.end();
}
bool QtTcpTransport::sendRequest(const Frame &request, std::uint16_t expectedType) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (terminal_ || !requestMode_ || !handshaken_ || awaiting_ || request.requestId <= lastRequestId_) return false;
    try {
        auto bytes = encode(request);
        if (request.payload.size() > exchange_->ack().maxControlPayload)
            throw std::runtime_error("negotiated control limit exceeded");
        lastRequestId_ = request.requestId;
        expectedType_ = expectedType;
        awaiting_ = true;
        socket_.setReadBufferSize(ControlLimit + 1);
        timer_.start(10000);
        if (socket_.write(bytes.data(), static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size())) {
            fail(tr("无法发送服务器查询请求。"));
            return false;
        }
        return true;
    } catch (const std::exception &) { fail(tr("服务器查询请求无效或超过协商限制。")); return false; }
}
void QtTcpTransport::disconnectFromServer() {
    Q_ASSERT(QThread::currentThread() == thread());
    terminal_ = true;
    handshaken_ = false;
    awaiting_ = false;
    timer_.stop();
    exchange_.reset();
    socket_.abort();
    received_.clear();
    emit statusChanged(tr("连接设置已更改，请重新检查连接。"));
}
void QtTcpTransport::fail(const QString &message) {
    if (terminal_) return;
    terminal_ = true;
    timer_.stop();
    lastError_ = message;
    socket_.abort();
    emit statusChanged(message);
    emit failed();
}
void QtTcpTransport::receive() {
    if (terminal_) { socket_.abort(); return; }
    const qint64 limit = handshaken_ ? ControlLimit : HandshakeLimit;
    const auto available = socket_.bytesAvailable();
    if (available > limit - received_.size()) { fail(tr("服务器消息超过大小限制。")); return; }
    received_ += socket_.read(available);
    try {
        if (received_.size() >= 32) {
            quint64 length = 0;
            for (int i = 20; i < 28; ++i) length = (length << 8) | static_cast<unsigned char>(received_[i]);
            if (length > static_cast<quint64>(limit - HeaderSize)) { fail(tr("服务器消息超过大小限制。")); return; }
        }
        auto result = decode({received_.constData(), static_cast<std::size_t>(received_.size())});
        if (result.status == DecodeStatus::Error) { fail(tr("协议帧无效。")); return; }
        if (result.status == DecodeStatus::NeedMore) return;
        if (result.consumed != static_cast<std::size_t>(received_.size())) { fail(tr("响应后出现意外数据。")); return; }
        received_.clear();
        if (!handshaken_) {
            exchange_->accept(result.frame);
            handshaken_ = true;
            terminal_ = !requestMode_;
            timer_.stop();
            emit statusChanged(tr("协议握手成功（版本 1），当前仅验证连接。"));
            emit ready();
        } else {
            if (!awaiting_ || result.frame.requestId != lastRequestId_ ||
                (result.frame.type != expectedType_ && result.frame.type != 15)) {
                fail(tr("服务器响应类型或请求标识不匹配。"));
                return;
            }
            awaiting_ = false;
            timer_.stop();
            emit frameReceived(result.frame);
        }
    } catch (const std::exception &) { fail(tr("服务器响应无效或协议不兼容。")); }
}
