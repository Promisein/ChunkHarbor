#pragma once
#include "chunkharbor/client_core/HandshakeExchange.h"
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <optional>

class QtTcpTransport final : public QObject {
    Q_OBJECT
  public:
    enum class HealthState { Unchecked, Checking, Reachable, Unreachable };
    Q_ENUM(HealthState)
    explicit QtTcpTransport(QObject *parent = nullptr);
    HealthState healthState() const { return healthState_; }
    QString healthStateText() const;
    void connectToServer(const QString &host, quint16 port);
    void connectForRequests(const QString &host, quint16 port, chunkharbor::protocol::Hello);
    bool sendRequest(const chunkharbor::protocol::Frame &, std::uint16_t expectedType);
    bool supportsCapability(const std::string &) const;
    QString lastError() const { return lastError_; }
    void disconnectFromServer();
  signals:
    void healthChanged();
    void statusChanged(const QString &text);
    void ready();
    void failed();
    void frameReceived(const chunkharbor::protocol::Frame &);

  private:
    void fail(const QString &message);
    void receive();
    void start(const QString &, quint16, chunkharbor::protocol::Hello, bool requestMode);
    void setHealthState(HealthState);
    QTcpSocket socket_;
    QTimer timer_;
    // Probe mode uses one short HELLO connection at a time, never an idle TCP link.
    QTimer healthTimer_;
    QString healthHost_;
    quint16 healthPort_ = 0;
    HealthState healthState_ = HealthState::Unchecked;
    QByteArray received_;
    std::optional<chunkharbor::client::HandshakeExchange> exchange_;
    bool terminal_ = true;
    bool requestMode_ = false, handshaken_ = false, awaiting_ = false;
    std::uint16_t expectedType_ = 0;
    std::uint64_t lastRequestId_ = 0;
    QString lastError_;
};
