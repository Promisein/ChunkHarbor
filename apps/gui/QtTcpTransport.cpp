#include "QtTcpTransport.h"
#include <QThread>
#include <exception>

namespace {
constexpr qint64 ReceiveLimit = chunkharbor::protocol::HeaderSize + chunkharbor::protocol::MaxHandshakePayload;
}
QtTcpTransport::QtTcpTransport(QObject* parent) : QObject(parent), socket_(this), timer_(this) {
    socket_.setReadBufferSize(ReceiveLimit + 1);
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] { fail(tr("Connection or handshake timed out (5 seconds).")); });
    connect(&socket_, &QTcpSocket::connected, this, [this] {
        if (terminal_) return;
        emit statusChanged(tr("Connected; checking protocol..."));
        try {
            const auto bytes = chunkharbor::protocol::encode(exchange_->request());
            if (socket_.write(bytes.data(), static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size()))
                fail(tr("Could not queue the handshake."));
        } catch (const std::exception&) { fail(tr("Could not create the handshake.")); }
    });
    connect(&socket_, &QTcpSocket::readyRead, this, &QtTcpTransport::receive);
    connect(&socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        fail(tr("Connection failed: %1").arg(socket_.errorString()));
    });
    connect(&socket_, &QTcpSocket::disconnected, this, [this] {
        if (exchange_ && exchange_->ready()) emit statusChanged(tr("Disconnected. Connect again to check the server."));
        else fail(tr("Server disconnected before completing the handshake."));
    });
}
void QtTcpTransport::connectToServer(const QString& host, quint16 port) {
    Q_ASSERT(QThread::currentThread() == thread());
    terminal_ = true;
    socket_.abort();
    received_.clear();
    chunkharbor::protocol::Hello offer;
    offer.clientName = "chunkharbor-gui";
    exchange_.emplace(std::move(offer));
    terminal_ = false;
    emit statusChanged(tr("Connecting..."));
    timer_.start(5000);
    socket_.connectToHost(host, port);
}
void QtTcpTransport::fail(const QString& message) {
    if (terminal_) return;
    terminal_ = true;
    timer_.stop();
    socket_.abort();
    emit statusChanged(message);
    emit failed();
}
void QtTcpTransport::receive() {
    if (terminal_) { socket_.abort(); return; }
    const auto available = socket_.bytesAvailable();
    if (available > ReceiveLimit - received_.size()) { fail(tr("Server handshake exceeded the size limit.")); return; }
    received_ += socket_.read(available);
    try {
        // Reject advertised oversized handshake lengths before waiting for a body.
        if (received_.size() >= 32) {
            quint64 length = 0;
            for (int i = 20; i < 28; ++i) length = (length << 8) | static_cast<unsigned char>(received_[i]);
            if (length > chunkharbor::protocol::MaxHandshakePayload) { fail(tr("Server handshake exceeded the size limit.")); return; }
        }
        const auto result = chunkharbor::protocol::decode({received_.constData(), static_cast<std::size_t>(received_.size())});
        if (result.status == chunkharbor::protocol::DecodeStatus::Error) { fail(tr("Invalid protocol frame.")); return; }
        if (result.status == chunkharbor::protocol::DecodeStatus::NeedMore) return;
        if (result.consumed != static_cast<std::size_t>(received_.size())) { fail(tr("Unexpected data after the handshake.")); return; }
        exchange_->accept(result.frame);
        terminal_ = true;
        timer_.stop();
        received_.clear();
        emit statusChanged(tr("Protocol ready (version 1). Connection prototype only."));
        emit ready();
    } catch (const std::exception&) { fail(tr("Server handshake was rejected: invalid response or incompatible protocol.")); }
}
