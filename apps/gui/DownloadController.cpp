#include "DownloadController.h"
#include "DownloadOutput.h"
#include "DownloadTransfer.h"
#include "chunkharbor/client_core/HandshakeExchange.h"
#include "chunkharbor/protocol/Download.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QTcpSocket>
#include <QNetworkProxy>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <stdexcept>

struct DownloadControl {
    // 0=running, 1=cancelled, 2=publishing, 3=published. CAS is the commit point:
    // cancellation never waits on disk I/O in the GUI thread.
    std::atomic_int phase{0};
    std::atomic<std::uint64_t> latestReceived{0};
    std::atomic_bool progressPending{false};
    void check() const { if (phase == 1) throw std::runtime_error("cancelled"); }
};
namespace {
using namespace chunkharbor::protocol;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
class DownloadPeer {
    QTcpSocket socket_;
    const std::function<void()> &check_;
  public:
    DownloadPeer(const QString &host, quint16 port, const std::function<void()> &check) : check_(check) {
        socket_.setProxy(QNetworkProxy::NoProxy);
        socket_.setReadBufferSize(HeaderSize + DataChunkHeaderSize + MaxDownloadChunkSize + 1);
        socket_.connectToHost(host, port, QIODevice::ReadWrite, QAbstractSocket::IPv4Protocol);
        QElapsedTimer clock;
        clock.start();
        while (socket_.state() != QAbstractSocket::ConnectedState) {
            check_();
            require(clock.elapsed() < 5000 && socket_.state() != QAbstractSocket::UnconnectedState,
                    "connection failed or timed out");
            socket_.waitForConnected(50);
        }
    }
    ~DownloadPeer() { socket_.abort(); }
    Frame exchange(const Frame &request, std::uint16_t expected) {
        check_();
        auto wire = encode(request);
        QElapsedTimer clock;
        clock.start();
        require(socket_.write(wire.data(), static_cast<qint64>(wire.size())) == static_cast<qint64>(wire.size()),
                "socket write failed");
        while (socket_.bytesToWrite()) {
            check_();
            require(clock.elapsed() < 10000 && socket_.state() == QAbstractSocket::ConnectedState,
                    "socket write timeout or disconnected");
            socket_.waitForBytesWritten(50);
        }
        const auto limit = expected == 14 ? DataChunkHeaderSize + MaxDownloadChunkSize : MaxControlPayload;
        std::string input;
        for (;;) {
            check_();
            auto parsed = decode(input);
            require(parsed.status != DecodeStatus::Error, "invalid response frame");
            if (input.size() >= HeaderSize) {
                std::uint64_t length = 0;
                for (int i = 20; i < 28; ++i) length = (length << 8) | static_cast<unsigned char>(input[i]);
                require(length <= limit, "response payload exceeds limit");
            }
            if (parsed.status == DecodeStatus::Complete) {
                require(parsed.consumed == input.size() && !socket_.bytesAvailable(), "unexpected trailing response");
                require(parsed.frame.requestId == request.requestId, "response request ID mismatch");
                if (parsed.frame.type == 15) {
                    const auto error = decodeUploadError(parsed.frame.payload);
                    throw std::runtime_error(error.code + ": " + error.message);
                }
                require(parsed.frame.type == expected, "unexpected response type");
                return std::move(parsed.frame);
            }
            require(clock.elapsed() < 10000, "download request timed out");
            require(input.size() < HeaderSize + limit, "response exceeds buffer limit");
            if (socket_.bytesAvailable()) {
                const auto bytes = socket_.read(static_cast<qint64>(HeaderSize + limit - input.size()));
                input.append(bytes.constData(), static_cast<std::size_t>(bytes.size()));
            } else {
                require(socket_.state() == QAbstractSocket::ConnectedState, "server disconnected before response completed");
                socket_.waitForReadyRead(50);
            }
        }
    }
};
QString failure(const std::exception &e) {
    const auto detail = QString::fromUtf8(e.what());
    if (detail.startsWith("VERSION_NOT_FOUND"))
        return QStringLiteral("下载失败：所选路径的历史版本不存在。（%1）").arg(detail);
    if (detail.startsWith("VERSION_UNAVAILABLE"))
        return QStringLiteral("下载失败：所选历史版本的数据对象或元数据不可用。（%1）").arg(detail);
    if (detail.startsWith("CHECKSUM_MISMATCH"))
        return QStringLiteral("下载失败：服务端版本数据校验失败。（%1）").arg(detail);
    if (detail.contains("VERSION_CONFLICT") || detail.contains("metadata mismatch"))
        return QStringLiteral("下载拒绝：文件版本或元数据已变化，请刷新列表后重试。（%1）").arg(detail);
    if (detail.contains("checksum")) return QStringLiteral("下载失败：SHA-256 校验不一致，未发布目标文件。（%1）").arg(detail);
    if (detail.contains("target") || detail.contains("publish"))
        return QStringLiteral("下载失败：目标已存在或无法安全发布；不会覆盖已有文件。（%1）").arg(detail);
    return QStringLiteral("下载失败：%1").arg(detail);
}
}
void gui::receiveDownload(const QString &host, quint16 port, const ListedFile &file,
    DownloadSelection selection, const std::function<void()> &check,
    const std::function<void(std::string_view)> &write, const std::function<void(quint64)> &progress) {
    const bool historical = selection == DownloadSelection::Historical;
    const BeginDownload request{file.path, file.versionId};
    const auto begin = historical ? encodeBeginHistoricalDownload(request) : encodeBeginDownload(request);
    const auto *capability = historical ? HistoricalDownloadCapability : "download_v1";
    require(file.size <= MaxDownloadFileSize, "file exceeds 64MiB download limit");
    check();
    DownloadPeer peer(host, port, check);
    Hello offer;
    offer.clientName = "chunkharbor-gui-download";
    offer.capabilities = {"handshake", capability};
    chunkharbor::client::HandshakeExchange handshake(offer, 1);
    handshake.accept(peer.exchange(handshake.request(), 2));
    const auto &ack = handshake.ack();
    require(std::find(ack.capabilities.begin(), ack.capabilities.end(), capability) != ack.capabilities.end(),
            historical ? "server does not support historical_download_v1" : "server does not support download_v1");
    const std::uint16_t openType = historical ? BeginHistoricalDownloadType : 13;
    require(begin.size() <= ack.maxControlPayload, "download request exceeds negotiated limit");
    const auto ready = decodeDownloadReady(peer.exchange({openType, 2, begin}, 32).payload);
    require(ready.path == file.path && ready.versionId == file.versionId && ready.size == file.size &&
            ready.sha256 == file.sha256, "download metadata mismatch");
    const auto maxData = ack.maxDataPayload > DataChunkHeaderSize
        ? std::min<std::uint64_t>(MaxDownloadChunkSize, ack.maxDataPayload - DataChunkHeaderSize) : 0;
    require(!ready.size || maxData, "server data limit too small");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::uint64_t offset = 0, requestId = 3;
    while (offset < ready.size) {
        check();
        const auto length = static_cast<std::uint32_t>(std::min<std::uint64_t>(maxData, ready.size - offset));
        const auto get = encodeGetDownloadChunk({ready.versionId, offset, length});
        require(get.size() <= ack.maxControlPayload, "chunk request exceeds negotiated limit");
        const auto chunk = decodeDownloadChunk(peer.exchange({33, requestId++, get}, 14).payload);
        require(chunk.versionId == ready.versionId && chunk.offset == offset && chunk.data.size() == length,
                "download chunk identity/range mismatch");
        const QByteArrayView bytes(chunk.data.data(), static_cast<qsizetype>(chunk.data.size()));
        const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        require(std::equal(chunk.sha256.begin(), chunk.sha256.end(),
            reinterpret_cast<const unsigned char *>(digest.constData())), "chunk checksum mismatch");
        check();
        write(chunk.data);
        hash.addData(bytes);
        offset += length;
        progress(offset);
    }
    require(hash.result().toHex().toStdString() == ready.sha256, "whole-file checksum mismatch");
    check();
}
DownloadController::DownloadController(QObject *parent) : QObject(parent) {}
DownloadController::~DownloadController() {
    cancel();
    if (worker_.joinable()) worker_.join();
}
void DownloadController::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!busy_ || !control_) return;
    int running = 0;
    if (!control_->phase.compare_exchange_strong(running, 1)) return;
    status_ = tr("正在取消下载并清理临时文件…");
    emit changed();
}
bool DownloadController::start(QString host, quint16 port, ListedFile file, QString target,
                               DownloadSelection selection) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (busy_) return false;
    if (worker_.joinable()) worker_.join();
    // Persist exactly the user's selected endpoint and version; never query a new current version.
    if (host.isEmpty() || !port || target.isEmpty() || file.versionId.empty()) return false;
    busy_ = true;
    succeeded_ = false;
    received_ = 0;
    total_ = file.size;
    fileName_ = QString::fromUtf8(file.path);
    const bool historical = selection == DownloadSelection::Historical;
    historicalVersion_ = historical ? QString::fromStdString(file.versionId) : QString{};
    status_ = tr("正在准备下载：%1").arg(fileName_);
    control_ = std::make_shared<DownloadControl>();
    emit changed();
    auto control = control_;
    try {
        worker_ = std::thread([this, host, port, file = std::move(file), target, control, historical] {
            bool success = false;
            QString status;
            try {
                control->check();
                DownloadOutput output(target);
                gui::receiveDownload(host, port, file,
                    historical ? DownloadSelection::Historical : DownloadSelection::Current,
                    [&] { control->check(); }, [&](std::string_view bytes) { output.write(bytes); },
                    [&](quint64 offset) {
                        control->latestReceived = offset;
                        if (!control->progressPending.exchange(true))
                            QMetaObject::invokeMethod(this, [this, control] {
                                control->progressPending = false;
                                received_ = control->latestReceived;
                                if (control->phase != 1) status_ = tr("正在下载：%1").arg(fileName_);
                                emit changed();
                            }, Qt::QueuedConnection);
                    });
                output.flush();
                int running = 0;
                require(control->phase.compare_exchange_strong(running, 2), "cancelled");
                output.publish();
                control->phase = 3;
                success = true;
                status = QStringLiteral("下载成功：SHA-256 校验通过，已保存到 %1").arg(target);
            } catch (const std::exception &e) {
                status = control->phase == 1 ? QStringLiteral("下载已取消；未发布目标文件。") : failure(e);
            }
            // Output and socket have already been destroyed before the terminal callback.
            QMetaObject::invokeMethod(this, [this, success, status, control] {
                busy_ = false;
                succeeded_ = success;
                received_ = control->latestReceived;
                status_ = status;
                emit changed();
                emit finished(success);
            }, Qt::QueuedConnection);
        });
    } catch (const std::exception &e) {
        busy_ = false;
        status_ = failure(e);
        emit changed();
        emit finished(false);
    }
    return true;
}
