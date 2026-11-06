#include "SshTunnelManager.h"
#include "QtTcpTransport.h"
#include <QDir>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <algorithm>
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <vector>
#endif
namespace {
bool valid(const SshTunnelConfig &c) {
    static const QRegularExpression name("\\A[A-Za-z0-9_][A-Za-z0-9_.-]{0,252}\\z");
    static const QRegularExpression user("\\A[A-Za-z_][A-Za-z0-9_.-]{0,63}\\z");
    return name.match(c.host).hasMatch() && name.match(c.targetHost).hasMatch() &&
        (c.user.isEmpty() || user.match(c.user).hasMatch()) && c.sshPort && c.localPort && c.targetPort;
}
// 1: this child owns the explicit loopback listener; 0: not yet listening;
// -1: foreign listener; -2: ownership cannot be established. Fail closed.
int owner(quint16 port, qint64 pid) {
#ifdef Q_OS_WIN
    DWORD size = 0;
    auto code = GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    for (int attempt = 0; attempt < 3 && code == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        if (size > 1024 * 1024 || size < sizeof(DWORD)) return -2;
        std::vector<unsigned char> data(size);
        code = GetExtendedTcpTable(data.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
        if (code != NO_ERROR) continue;
        const auto *table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID *>(data.data());
        if (table->dwNumEntries > (data.size() - sizeof(DWORD)) / sizeof(MIB_TCPROW_OWNER_PID)) return -2;
        bool owned = false;
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const auto &row = table->table[i];
            if (ntohs(static_cast<u_short>(row.dwLocalPort)) != port ||
                (row.dwLocalAddr != htonl(INADDR_LOOPBACK) && row.dwLocalAddr != 0)) continue;
            if (row.dwOwningPid != static_cast<DWORD>(pid) || row.dwLocalAddr != htonl(INADDR_LOOPBACK)) return -1;
            owned = true;
        }
        return owned ? 1 : 0;
    }
#else
    Q_UNUSED(port); Q_UNUSED(pid);
#endif
    return -2;
}
QString sshExecutable() {
#ifdef Q_OS_WIN
    const auto system = QDir(qEnvironmentVariable("WINDIR", "C:/Windows")).filePath("System32/OpenSSH/ssh.exe");
    if (QFileInfo::exists(system)) return system;
    return QStandardPaths::findExecutable("ssh.exe");
#else
    return {};
#endif
}
}
bool SshTunnelConfig::operator==(const SshTunnelConfig &o) const {
    return host == o.host && user == o.user && targetHost == o.targetHost && sshPort == o.sshPort &&
        localPort == o.localPort && targetPort == o.targetPort;
}
SshTunnelManager::SshTunnelManager(QtTcpTransport *transport, QObject *parent)
    : SshTunnelManager(transport, SshTunnelRuntime{}, parent) {}
SshTunnelManager::SshTunnelManager(QtTcpTransport *transport, SshTunnelRuntime runtime, QObject *parent)
    : QObject(parent), transport_(transport), runtime_(std::move(runtime)) {
    startup_.setSingleShot(true); kill_.setSingleShot(true); retry_.setSingleShot(true); stable_.setSingleShot(true);
    polling_.setInterval(100);
    connect(&startup_, &QTimer::timeout, this, [this] { fail(tr("SSH 建立超时，请检查主机、网络与密钥配置。"), true); });
    connect(&polling_, &QTimer::timeout, this, &SshTunnelManager::pollReady);
    connect(&kill_, &QTimer::timeout, this, [this] { if (running()) process_->kill(); });
    connect(&retry_, &QTimer::timeout, this, &SshTunnelManager::launch);
    connect(&stable_, &QTimer::timeout, this, [this] { retries_ = 0; });
    connect(transport_, &QtTcpTransport::healthChanged, this, &SshTunnelManager::healthChanged);
}
SshTunnelManager::~SshTunnelManager() {
    wanted_ = false;
    if (process_) { process_->disconnect(this); process_->kill(); }
#ifdef Q_OS_WIN
    if (job_) CloseHandle(reinterpret_cast<HANDLE>(job_));
#endif
}
bool SshTunnelManager::available() const { return wanted_ && ready_ && verified_ && running(); }
void SshTunnelManager::setState(State state, const QString &text) {
    state_ = state; status_ = text; emit changed();
}
void SshTunnelManager::clearAvailability() {
    const bool hadTunnel = ready_;
    ready_ = verified_ = false;
    if (hadTunnel && (state_ == State::Connected || state_ == State::CheckingServer || state_ == State::TunnelReady)) {
        const bool retrying = wanted_ && reconnectAllowed_ && end_ == End::None &&
            retries_ < std::clamp(runtime_.maxRetries, 0, 3);
        state_ = (end_ == End::Restart || end_ == End::Disabled) ? State::Stopping
            : retrying ? State::Reconnecting : State::Failed;
        status_ = tr("SSH 隧道已断开，正在收尾当前连接。");
    }
    transport_->disconnectFromServer();
    if (hadTunnel) emit tunnelLost();
    emit changed();
}
void SshTunnelManager::start(const SshTunnelConfig &config) {
    if (wanted_ && config_ == config) return;
    config_ = config;
    wanted_ = true; retries_ = 0; reconnectAllowed_ = true;
    halt(End::Restart);
}
void SshTunnelManager::retry() {
    if (state_ == State::Stopping) return;
    reconnectAllowed_ = true;
    if (!wanted_) { wanted_ = true; retries_ = 0; launch(); return; }
    if (state_ == State::Starting || state_ == State::Stopping || state_ == State::CheckingServer) return;
    retries_ = 0; retry_.stop();
    if (ready_ && running()) {
        pollReady();
        if (ready_) transport_->connectToServer("127.0.0.1", config_.localPort);
    } else if (!running()) launch();
}
void SshTunnelManager::stopRetries() { reconnectAllowed_ = false; retry_.stop(); stable_.stop(); }
void SshTunnelManager::stop() { wanted_ = false; stopRetries(); halt(End::Disabled); }
void SshTunnelManager::halt(End action) {
    startup_.stop(); polling_.stop(); retry_.stop(); stable_.stop();
    end_ = action;
    clearAvailability();
    if (running()) {
        if (action == End::Restart || action == End::Disabled) setState(State::Stopping, tr("正在关闭本程序的 SSH 隧道…"));
        process_->terminate();
        kill_.start(runtime_.stopMs);
    } else if (process_) exited(process_);
    else if (action == End::Restart) launch();
    else if (action == End::Retry) scheduleRetry();
    else if (action == End::Disabled) { setState(State::Disabled, tr("SSH 隧道已关闭。")); emit stopped(); }
}
void SshTunnelManager::launch() {
    if (!wanted_ || process_) return;
    retry_.stop(); end_ = End::None;
    diagnostic_.clear(); errorLine_.clear(); errorBytes_ = 0;
    if (!valid(config_)) { fail(tr("SSH 参数无效：请检查主机别名、用户、目标地址和端口。"), false); return; }
    const auto program = runtime_.executable.isEmpty() ? sshExecutable() : runtime_.executable;
    if (program.isEmpty() || !QFileInfo(program).isExecutable()) {
        fail(tr("未找到可执行的 ssh.exe，请安装 Windows OpenSSH 客户端。"), false); return;
    }
    QTcpServer reservation;
    reservation.setProxy(QNetworkProxy::NoProxy);
    if (!reservation.listen(QHostAddress::LocalHost, config_.localPort)) {
        fail(tr("本地端口 %1 已被占用。请关闭手工隧道或更换端口；不会复用或终止外部进程。").arg(config_.localPort), false);
        return;
    }
    reservation.close(); // SSH must bind itself; PID validation below closes the race in trust.
    auto *child = process_ = new QProcess(this);
    child->setStandardInputFile(QProcess::nullDevice());
    child->setStandardOutputFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    child->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
        args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        args->startupInfo->wShowWindow = SW_HIDE;
    });
#endif
    connect(child, &QProcess::started, this, [this, child] {
        if (child != process_) return;
#ifdef Q_OS_WIN
        const auto job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        const auto handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(child->processId()));
        const bool contained = job && handle && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
            AssignProcessToJobObject(job, handle);
        if (handle) CloseHandle(handle);
        if (!contained) {
            if (job) CloseHandle(job);
            if (end_ == End::None) fail(tr("无法受控管理 SSH 子进程，已停止启动。"), false);
            else child->kill();
            return;
        }
        job_ = reinterpret_cast<quintptr>(job);
#endif
        if (end_ != End::None) { child->terminate(); return; }
        polling_.start(); pollReady();
    });
    connect(child, &QProcess::readyReadStandardError, this, &SshTunnelManager::readErrors);
    connect(child, &QProcess::errorOccurred, this, [this, child](QProcess::ProcessError error) {
        if (child != process_) return;
        if (error == QProcess::FailedToStart) {
            if (end_ == End::None) { diagnostic_ = tr("ssh.exe 启动失败，请检查 OpenSSH 安装和执行权限。"); end_ = End::Failed; }
            exited(child);
        }
    });
    connect(child, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, child] { exited(child); });
    QStringList arguments{"-N", "-T", "-o", "BatchMode=yes", "-o", "ExitOnForwardFailure=yes", "-o", "ConnectTimeout=5",
        "-o", "ConnectionAttempts=1", "-o", "ServerAliveInterval=30", "-o", "ServerAliveCountMax=3",
        "-o", "StrictHostKeyChecking=yes", "-o", "NumberOfPasswordPrompts=0", "-o", "ControlMaster=no",
        "-o", "ControlPath=none", "-o", "ControlPersist=no", "-o", "ForkAfterAuthentication=no",
        "-o", "PermitLocalCommand=no", "-p", QString::number(config_.sshPort)};
    if (!config_.user.isEmpty()) arguments << "-l" << config_.user;
    arguments << "-L" << QString("127.0.0.1:%1:%2:%3").arg(config_.localPort).arg(config_.targetHost).arg(config_.targetPort)
        << "--" << config_.host;
    ++starts_;
    setState(State::Starting, tr("正在建立 SSH 隧道（%1）…").arg(config_.host));
    startup_.start(runtime_.startupMs);
    child->start(program, arguments, QIODevice::ReadOnly);
}
void SshTunnelManager::pollReady() {
    if (!wanted_ || !running() || end_ != End::None) return;
    const int owns = owner(config_.localPort, process_->processId());
    if (owns < 0) { fail(owns == -1 ? tr("本地转发端口属于其他进程，已拒绝连接。") : tr("无法验证 SSH 监听端口的进程归属。"), false); return; }
    if (owns != 1 || ready_) return;
    ready_ = true; polling_.stop(); startup_.stop();
    setState(State::TunnelReady, tr("SSH 隧道已建立，尚未确认备份服务。"));
    transport_->connectToServer("127.0.0.1", config_.localPort);
}
void SshTunnelManager::healthChanged() {
    if (!wanted_ || !ready_ || !running() || end_ != End::None) return;
    const auto health = transport_->healthState();
    if (health == QtTcpTransport::HealthState::Checking) setState(State::CheckingServer, tr("SSH 已建立，正在检查 ChunkHarbor HELLO…"));
    else if (health == QtTcpTransport::HealthState::Reachable) {
        if (owner(config_.localPort, process_->processId()) != 1) { fail(tr("SSH 端口归属发生变化，已拒绝连接。"), false); return; }
        verified_ = true;
        if (!stable_.isActive()) stable_.start(runtime_.stableMs);
        setState(State::Connected, tr("SSH 隧道和 ChunkHarbor HELLO 检查成功。"));
    } else if (health == QtTcpTransport::HealthState::Unreachable) {
        verified_ = false;
        setState(State::Failed, tr("SSH 隧道已建立，但备份服务不可用：%1").arg(transport_->lastError()));
    }
}
void SshTunnelManager::readErrors() {
    if (!process_) return;
    process_->setReadChannel(QProcess::StandardError);
    // An old process being stopped must not overwrite Restart/Disabled with
    // a late authentication error. Drain a bounded amount, never reinterpret it.
    if (end_ != End::None) { process_->read(65536); return; }
    while (process_ && process_->bytesAvailable()) {
        const auto data = process_->read(4096);
        errorBytes_ += static_cast<int>(data.size());
        if (errorBytes_ > 65536) { fail(tr("SSH 错误输出超过安全上限，已停止该进程。"), false); return; }
        errorLine_ += data;
        const auto lower = errorLine_.toLower();
        QString error;
        if (lower.contains("permission denied") || lower.contains("sign_and_send_pubkey"))
            error = tr("SSH 密钥认证失败。请先完成一次性密钥登录，或将密钥加载到 ssh-agent；应用不接收密码。");
        else if (lower.contains("host key verification failed") || lower.contains("remote host identification has changed"))
            error = tr("SSH 主机密钥未受信任或已变化。请先在终端核对服务器指纹并完成一次性信任配置。");
        else if (lower.contains("address already in use") || lower.contains("cannot listen to port"))
            error = tr("SSH 本地转发端口被占用，请关闭手工隧道或更换端口。");
        else if (lower.contains("bad configuration") || lower.contains("bad configuration option"))
            error = tr("OpenSSH 配置无效，请检查本机 SSH 配置。");
        if (!error.isEmpty()) { fail(error, false); return; }
        if (lower.contains("connection timed out") || lower.contains("connection refused") || lower.contains("could not resolve hostname"))
            diagnostic_ = tr("SSH 主机不可达，请检查主机地址、SSH 端口与虚拟机网络。");
        if (errorLine_.size() > 8192) errorLine_ = errorLine_.right(4096);
    }
}
void SshTunnelManager::fail(const QString &message, bool transient) {
    diagnostic_ = message;
    setState(State::Failed, message);
    halt(transient && wanted_ && reconnectAllowed_ && retries_ < std::clamp(runtime_.maxRetries, 0, 3) ? End::Retry : End::Failed);
}
void SshTunnelManager::exited(QProcess *child) {
    if (child != process_) return;
    readErrors();
    if (child != process_) return;
    const auto action = end_;
    startup_.stop(); polling_.stop(); kill_.stop(); stable_.stop();
    process_ = nullptr;
    child->disconnect(this); child->deleteLater();
#ifdef Q_OS_WIN
    if (job_) { CloseHandle(reinterpret_cast<HANDLE>(job_)); job_ = 0; }
#endif
    clearAvailability();
    if (action == End::Restart && wanted_) { QTimer::singleShot(0, this, &SshTunnelManager::launch); return; }
    if (action == End::Disabled || !wanted_) { setState(State::Disabled, tr("SSH 隧道已关闭。")); emit stopped(); return; }
    if (reconnectAllowed_ && (action == End::Retry || (action == End::None && retries_ < std::clamp(runtime_.maxRetries, 0, 3)))) scheduleRetry();
    else setState(State::Failed, diagnostic_.isEmpty() ? tr("SSH 已退出，重连次数已用尽，请检查配置后重试。") : diagnostic_);
}
void SshTunnelManager::scheduleRetry() {
    if (!wanted_ || process_) return;
    const int delay = std::min(8000, std::max(1, runtime_.retryBaseMs) * (1 << std::min(retries_, 3)));
    ++retries_;
    setState(State::Reconnecting, tr("%1 将在 %2 秒后重连（%3/%4）。")
        .arg(diagnostic_.isEmpty() ? tr("SSH 连接已断开。") : diagnostic_).arg(delay / 1000.0).arg(retries_).arg(runtime_.maxRetries));
    retry_.start(delay);
}
