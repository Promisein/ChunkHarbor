#include "DeltaClient.h"
#include "chunkharbor/client_core/HandshakeExchange.h"
#include "chunkharbor/protocol/BeginUpload.h"
#include "chunkharbor/protocol/ChunkAck.h"
#include "chunkharbor/protocol/SessionStatus.h"
#include "chunkharbor/protocol/UploadControl.h"
#include "chunkharbor/protocol/UploadPlan.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTcpSocket>
#include <QThread>
#include <algorithm>
#include <iostream>
#include <stdexcept>
namespace {
using namespace chunkharbor::protocol;
std::string narrow(const QByteArray &bytes) {
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}
std::string sidHex(const std::array<std::uint8_t, 16> &id) {
    const char *digits = "0123456789abcdef";
    std::string result;
    for (auto byte : id) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}
std::array<std::uint8_t, 16> parseSid(const QString &text) {
    auto bytes = text.toLatin1();
    if (bytes.size() != 32)
        throw std::runtime_error("invalid saved session ID");
    std::array<std::uint8_t, 16> id{};
    for (int i = 0; i < 32; ++i) {
        const int n = QByteArray("0123456789abcdef").indexOf(bytes[i]);
        if (n < 0)
            throw std::runtime_error("invalid saved session ID");
        id[i / 2] = static_cast<std::uint8_t>((id[i / 2] << 4) | n);
    }
    return id;
}
class Peer {
    QTcpSocket socket_;
    std::string input_;

  public:
    explicit Peer(quint16 port) {
        socket_.connectToHost(QHostAddress::LocalHost, port);
        if (!socket_.waitForConnected(5000))
            throw std::runtime_error("connect failed");
        Hello offer;
        offer.capabilities = {"handshake", "upload"};
        chunkharbor::client::HandshakeExchange exchange(offer, 1);
        auto hello = exchange.request();
        auto reply = request(hello.type, hello.requestId, hello.payload);
        exchange.accept(reply);
        if (std::find(exchange.ack().capabilities.begin(), exchange.ack().capabilities.end(),
                      "upload") == exchange.ack().capabilities.end())
            throw std::runtime_error("server does not offer upload");
    }
    Frame request(std::uint16_t type, std::uint64_t id, std::string payload) {
        auto wire = encode({type, id, std::move(payload)});
        if (socket_.write(wire.data(), static_cast<qint64>(wire.size())) !=
            static_cast<qint64>(wire.size()))
            throw std::runtime_error("socket send failed");
        while (socket_.bytesToWrite() != 0)
            if (!socket_.waitForBytesWritten(30000))
                throw std::runtime_error("socket send timed out");
        for (;;) {
            auto result = decode(input_);
            if (result.status == DecodeStatus::Error)
                throw std::runtime_error("invalid server frame");
            if (result.status == DecodeStatus::Complete) {
                input_.erase(0, result.consumed);
                if (result.frame.requestId != id)
                    throw std::runtime_error("response ID mismatch");
                if (result.frame.type == 15) {
                    auto error = decodeUploadError(result.frame.payload);
                    throw std::runtime_error(error.code + ": " + error.message);
                }
                return result.frame;
            }
            if (input_.size() >= HeaderSize + MaxControlPayload)
                throw std::runtime_error("response too large");
            if (!socket_.waitForReadyRead(30000))
                throw std::runtime_error("server response timed out");
            input_ += narrow(socket_.readAll());
        }
    }
};
Frame expect(Peer &peer, std::uint16_t type, std::uint64_t id, std::string body,
             std::uint16_t responseType) {
    auto frame = peer.request(type, id, std::move(body));
    if (frame.type != responseType)
        throw std::runtime_error("unexpected response type");
    return frame;
}
QJsonObject loadState(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16384)
        throw std::runtime_error("cannot read saved session");
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        throw std::runtime_error("invalid saved session");
    return doc.object();
}
void saveState(const QString &path, const QJsonObject &value) {
    QSaveFile file(path);
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("cannot persist session ID");
}
QByteArray hashFile(QFile &file) {
    if (!file.seek(0))
        throw std::runtime_error("source seek failed");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        auto bytes = file.read(65536);
        if (bytes.isEmpty() && file.error() != QFile::NoError)
            throw std::runtime_error("source read failed");
        hash.addData(bytes);
    }
    if (!file.seek(0))
        throw std::runtime_error("source seek failed");
    return hash.result().toHex();
}
struct Options {
    QString file, path, state, sessionId;
    int port = 8787, chunkSize = 262144, delayMs = 0;
};
Options options(const QStringList &args) {
    Options result;
    for (int i = 2; i < args.size(); i += 2) {
        if (i + 1 >= args.size())
            throw std::runtime_error("missing option value");
        const auto key = args[i], value = args[i + 1];
        if (key == "--file")
            result.file = value;
        else if (key == "--path")
            result.path = value;
        else if (key == "--state")
            result.state = value;
        else if (key == "--session-id")
            result.sessionId = value;
        else if (key == "--port")
            result.port = value.toInt();
        else if (key == "--chunk-size")
            result.chunkSize = value.toInt();
        else if (key == "--delay-ms")
            result.delayMs = value.toInt();
        else
            throw std::runtime_error("unknown option");
    }
    if (result.port < 1 || result.port > 65535 || result.chunkSize < 1 ||
        result.chunkSize > static_cast<int>(MaxChunkDataSize) || result.delayMs < 0 ||
        result.delayMs > 60000)
        throw std::runtime_error("invalid numeric option");
    return result;
}
void run(const QStringList &args) {
    if (args.size() < 2 || args[1] == "--help") {
        std::cout << "Delta: (backup|preview) --file SOURCE --path LOGICAL --state STATE "
                     "[--chunk-size N] [--port N]\nDirectory: (backup|preview) --directory ROOT "
                     "--prefix LOGICAL --state-dir DIR [--exclude COMPONENT]\nPreview conditions: "
                     "--base-version ID|absent --source-sha256 HASH\n";
        std::cout << "Windows CLI: upload --file SOURCE --path LOGICAL --state STATE [--port 8787] "
                     "[--chunk-size 262144] [--delay-ms 0]\n"
                     "             status (--state STATE | --session-id ID) [--port 8787]\n";
        return;
    }
    const bool upload = args[1] == "upload";
    if (!upload && args[1] != "status")
        throw std::runtime_error("unknown command");
    auto opt = options(args);
    if (upload && (opt.file.isEmpty() || opt.path.isEmpty()))
        throw std::runtime_error("upload requires --file and --path");
    if (upload && (opt.state.isEmpty() || !opt.sessionId.isEmpty()))
        throw std::runtime_error("upload requires --state and does not accept --session-id");
    if (!upload && (opt.state.isEmpty() == opt.sessionId.isEmpty()))
        throw std::runtime_error("status requires exactly one of --state or --session-id");
    QFile source(opt.file);
    QByteArray digest;
    qint64 size = 0;
    if (upload) {
        if (!QFileInfo(source).isFile() || !source.open(QIODevice::ReadOnly))
            throw std::runtime_error("source is not a readable regular file");
        size = source.size();
        if (size < 0 || size > 64 * 1024 * 1024)
            throw std::runtime_error("source exceeds 64 MiB");
        digest = hashFile(source);
    }
    Peer peer(static_cast<quint16>(opt.port));
    std::uint64_t requestId = 2;
    if (!upload && !opt.sessionId.isEmpty()) {
        const auto session = parseSid(opt.sessionId);
        auto reply = peer.request(7, requestId, encodeQuerySession({session}));
        if (reply.type == 10) {
            auto result = decodeCommitResult(reply.payload);
            if (result.sessionId != session)
                throw std::runtime_error("committed session mismatch");
            std::cout << "COMMITTED session=" << sidHex(session) << " sha256=" << result.sha256
                      << '\n';
            return;
        }
        if (reply.type != 8)
            throw std::runtime_error("unexpected session status");
        auto status = decodeSessionStatus(reply.payload);
        if (status.sessionId != session)
            throw std::runtime_error("session status mismatch");
        std::uint64_t missing = 0;
        for (std::uint32_t index = 0; index < status.blockCount; ++index)
            missing += (status.missing[index / 8] >> (index % 8)) & 1U;
        std::cout << "SESSION session=" << sidHex(session) << " MISSING_BLOCKS=" << missing << '\n';
        return;
    }
    UploadPlan plan;
    bool resumedSession = false;
    if (QFile::exists(opt.state)) {
        const auto saved = loadState(opt.state);
        auto session = parseSid(saved.value("session_id").toString());
        if (upload && (saved.value("file").toString() != QFileInfo(source).absoluteFilePath() ||
                       saved.value("path").toString() != opt.path ||
                       saved.value("size").toInteger(-1) != size ||
                       saved.value("sha256").toString().toLatin1() != digest ||
                       saved.value("chunk_size").toInt() != opt.chunkSize))
            throw std::runtime_error("saved session does not match source snapshot");
        auto reply = peer.request(7, requestId++, encodeQuerySession({session}));
        if (reply.type == 10) {
            auto result = decodeCommitResult(reply.payload);
            if (result.sessionId != session)
                throw std::runtime_error("committed session mismatch");
            std::cout << "ALREADY COMMITTED session=" << sidHex(session) << '\n';
            if (upload)
                QFile::remove(opt.state);
            return;
        }
        if (reply.type != 8)
            throw std::runtime_error("unexpected session status");
        auto status = decodeSessionStatus(reply.payload);
        plan.sessionId = status.sessionId;
        plan.chunkSize = status.chunkSize;
        for (std::uint32_t index = 0; index < status.blockCount;) {
            if (!(status.missing[index / 8] & (1U << (index % 8)))) {
                ++index;
                continue;
            }
            const auto begin = index;
            while (index < status.blockCount && (status.missing[index / 8] & (1U << (index % 8))))
                ++index;
            plan.missingRanges.push_back({begin, index});
        }
        resumedSession = true;
        if (upload)
            validateSessionStatusForFile(status, static_cast<std::uint64_t>(size),
                                         static_cast<std::uint32_t>(opt.chunkSize));
        if (plan.sessionId != session)
            throw std::runtime_error("session status mismatch");
        std::cout << "RESUME session=" << sidHex(session) << '\n';
    } else {
        if (!upload)
            throw std::runtime_error("saved session does not exist");
        BeginUpload begin{narrow(opt.path.toUtf8()), static_cast<std::uint64_t>(size), 0,
                          static_cast<std::uint32_t>(opt.chunkSize), narrow(digest)};
        plan = decodeUploadPlan(expect(peer, 3, requestId++, encodeBeginUpload(begin), 4).payload);
        saveState(opt.state, {{"session_id", QString::fromStdString(sidHex(plan.sessionId))},
                              {"file", QFileInfo(source).absoluteFilePath()},
                              {"path", opt.path},
                              {"size", size},
                              {"sha256", QString::fromLatin1(digest)},
                              {"chunk_size", opt.chunkSize}});
        std::cout << "NEW session=" << sidHex(plan.sessionId) << '\n';
    }
    if (!upload) {
        std::uint64_t missing = 0;
        for (auto range : plan.missingRanges)
            missing += range.end - range.begin;
        std::cout << "MISSING_BLOCKS=" << missing << '\n';
        return;
    }
    if (!resumedSession)
        validateUploadPlanForFile(plan, static_cast<std::uint64_t>(size),
                                  static_cast<std::uint32_t>(opt.chunkSize));
    std::uint64_t sentBlocks = 0;
    for (const auto range : plan.missingRanges)
        for (auto index = range.begin; index < range.end; ++index) {
            const auto offset = index * static_cast<std::uint64_t>(opt.chunkSize);
            const auto length = static_cast<qint64>(
                std::min<std::uint64_t>(opt.chunkSize, static_cast<std::uint64_t>(size) - offset));
            if (!source.seek(static_cast<qint64>(offset)))
                throw std::runtime_error("source seek failed");
            auto data = source.read(length);
            if (data.size() != length)
                throw std::runtime_error("source read failed");
            auto bytes = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
            DataChunk chunk{plan.sessionId, offset, {}, narrow(data)};
            std::copy(bytes.begin(), bytes.end(), chunk.sha256.begin());
            auto ack =
                decodeChunkAck(expect(peer, 5, requestId++, encodeDataChunk(chunk), 6).payload);
            validateChunkAckForChunk(ack, chunk);
            ++sentBlocks;
            std::cout << "ACK blocks_sent_this_run=" << sentBlocks << '\n';
            if (opt.delayMs)
                QThread::msleep(static_cast<unsigned long>(opt.delayMs));
        }
    if (hashFile(source) != digest)
        throw std::runtime_error("source changed during upload");
    auto result = decodeCommitResult(
        expect(peer, 9, requestId++, encodeCommitUpload({plan.sessionId}), 10).payload);
    if (result.sessionId != plan.sessionId || result.path != narrow(opt.path.toUtf8()) ||
        result.size != static_cast<std::uint64_t>(size) || result.sha256 != narrow(digest))
        throw std::runtime_error("commit result mismatch");
    QFile::remove(opt.state);
    std::cout << "UPLOAD OK blocks_sent_this_run=" << sentBlocks << " sha256=" << digest.constData()
              << '\n';
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc > 1 && (std::string(argv[1]) == "backup" || std::string(argv[1]) == "preview")) {
            std::vector<QByteArray> values;
            std::vector<char *> pointers;
            for (const auto &value : app.arguments())
                values.push_back(value.toUtf8());
            for (auto &value : values)
                pointers.push_back(value.data());
            return runDeltaCommand(static_cast<int>(pointers.size()), pointers.data());
        }
        run(app.arguments());
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
