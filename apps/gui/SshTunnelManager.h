#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>

class QtTcpTransport;
struct SshTunnelConfig {
    QString host = "miniob-ubuntu", user, targetHost = "127.0.0.1";
    quint16 sshPort = 22, localPort = 18787, targetPort = 8787;
    bool operator==(const SshTunnelConfig &other) const;
};
// Injectable execution/timing policy for isolated process tests, never credentials/settings.
struct SshTunnelRuntime {
    QString executable;
    int startupMs = 12000, stopMs = 1000, retryBaseMs = 1000, maxRetries = 3, stableMs = 60000;
};
class SshTunnelManager final : public QObject {
    Q_OBJECT
  public:
    enum class State { Disabled, Starting, TunnelReady, CheckingServer, Connected, Failed,
                       Reconnecting, Stopping };
    Q_ENUM(State)
    explicit SshTunnelManager(QtTcpTransport *, QObject *parent = nullptr);
    SshTunnelManager(QtTcpTransport *, SshTunnelRuntime, QObject *parent = nullptr);
    ~SshTunnelManager() override;
    void start(const SshTunnelConfig &);
    void retry();
    void stop();
    void stopRetries();
    State state() const { return state_; }
    QString status() const { return status_; }
    bool available() const;
    bool running() const { return process_ && process_->state() != QProcess::NotRunning; }
    qint64 processId() const { return process_ ? process_->processId() : 0; }
    int starts() const { return starts_; }
  signals:
    void changed();
    void tunnelLost();
    void stopped();
  private:
    enum class End { None, Disabled, Restart, Retry, Failed };
    void launch();
    void halt(End);
    void exited(QProcess *);
    void fail(const QString &, bool transient);
    void scheduleRetry();
    void pollReady();
    void readErrors();
    void healthChanged();
    void setState(State, const QString &);
    void clearAvailability();
    QtTcpTransport *transport_;
    SshTunnelRuntime runtime_;
    SshTunnelConfig config_;
    QProcess *process_ = nullptr;
    QTimer startup_, polling_, kill_, retry_, stable_;
    State state_ = State::Disabled;
    End end_ = End::None;
    QString status_, diagnostic_;
    QByteArray errorLine_;
    bool wanted_ = false, ready_ = false, verified_ = false, reconnectAllowed_ = true;
    int retries_ = 0, starts_ = 0, errorBytes_ = 0;
    quintptr job_ = 0;
};
