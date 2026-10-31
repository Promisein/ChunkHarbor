#pragma once
#include "chunkharbor/client_core/HandshakeExchange.h"
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <optional>

class QtTcpTransport final : public QObject {
    Q_OBJECT
public:
    explicit QtTcpTransport(QObject* parent = nullptr);
    void connectToServer(const QString& host, quint16 port);
signals:
    void statusChanged(const QString& text);
    void ready();
    void failed();
private:
    void fail(const QString& message);
    void receive();
    QTcpSocket socket_;
    QTimer timer_;
    QByteArray received_;
    std::optional<chunkharbor::client::HandshakeExchange> exchange_;
    bool terminal_ = true;
};
