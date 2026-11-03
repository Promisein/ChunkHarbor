#include "TransferTypes.h"
#include "chunkharbor/protocol/ChunkAck.h"
#include "chunkharbor/protocol/Handshake.h"
#include "chunkharbor/protocol/ManifestPage.h"
#include "chunkharbor/protocol/PlanPage.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
static void check(bool b) {
    if (!b)
        throw std::runtime_error("worker assertion failed");
}
// A real local TCP fake: validates the worker framing, persistence and recovery
// without claiming server-side durability. Real server verification is separate.
class FakeServer {
    struct Session {
        chunkharbor::protocol::BeginDelta begin;
        bool committed = false;
    };
    std::map<QString, Session> sessions;
    std::atomic_bool stopping{false};
    std::thread thread;

  public:
    quint16 port = 0;
    std::atomic_bool failBlock{false}, wrongId{false}, rejectPreviewBaseline{false};
    std::atomic_int blocks{0}, queries{0}, evilPlan{0};
    FakeServer() {
        std::promise<quint16> ready;
        auto future = ready.get_future();
        thread = std::thread([this, &ready] {
            using namespace chunkharbor::protocol;
            QTcpServer server;
            if (!server.listen(QHostAddress::LocalHost, 0)) {
                ready.set_value(0);
                return;
            }
            ready.set_value(server.serverPort());
            int serial = 0;
            while (!stopping) {
                if (!server.waitForNewConnection(100))
                    continue;
                std::unique_ptr<QTcpSocket> socket(server.nextPendingConnection());
                std::string input;
                std::uint64_t lastRequest = 0;
                while (!stopping && socket->state() == QAbstractSocket::ConnectedState) {
                    if (!socket->bytesAvailable()) {
                        socket->waitForReadyRead(100);
                        continue;
                    }
                    auto b = socket->readAll();
                    input.append(b.constData(), static_cast<size_t>(b.size()));
                    auto d = decode(input);
                    if (d.status == DecodeStatus::NeedMore)
                        continue;
                    if (d.status != DecodeStatus::Complete)
                        break;
                    input.erase(0, d.consumed);
                    auto q = d.frame;
                    if (!q.requestId || q.requestId <= lastRequest) {
                        socket->abort();
                        break;
                    }
                    lastRequest = q.requestId;
                    Frame r{0, q.requestId, {}};
                    if (wrongId.exchange(false)) {
                        auto wire = encode(
                            {2, q.requestId + 1,
                             encodeHelloAck(
                                 {1, "fake", {"handshake", "upload", "fixed_block_delta_v1"}})});
                        socket->write(wire.data(), static_cast<qint64>(wire.size()));
                        socket->waitForBytesWritten(100);
                        break;
                    }
                    if (q.type == 1) {
                        r.type = 2;
                        r.payload = encodeHelloAck(
                            {1, "fake", {"handshake", "upload", "fixed_block_delta_v1"}});
                    } else if (q.type == 24) {
                        auto begin = decodeBeginDelta(q.payload);
                        SessionId id{};
                        id[0] = static_cast<uint8_t>(++serial);
                        sessions[QString::fromLatin1(
                            QByteArray(reinterpret_cast<char *>(id.data()), 16).toHex())] = {begin,
                                                                                             false};
                        r.type = 25;
                        std::optional<VersionId> readyBase;
                        if (begin.mode == DeltaMode::Preview && rejectPreviewBaseline.exchange(false))
                            readyBase = VersionId{};
                        r.payload = encodeDeltaReady({id, readyBase, 1});
                    } else if (q.type == 18) {
                        auto p = decodeManifestPage(q.payload);
                        r.type = 26;
                        r.payload = encodeManifestAck(
                            {p.sessionId, p.pageIndex + 1, DeltaPhase::Collecting});
                    } else if (q.type == 19) {
                        auto p = decodeManifestDone(q.payload);
                        r.type = 26;
                        r.payload =
                            encodeManifestAck({p.sessionId, p.pageCount, DeltaPhase::PlanReady});
                    } else if (q.type == 27) {
                        auto p = decodeGetPlanPage(q.payload);
                        if (p.pageIndex == 0 && evilPlan.load()) {
                            const int evil = evilPlan.exchange(0);
                            auto session = p.sessionId;
                            std::optional<VersionId> base;
                            if (evil == 1)
                                ++session[0];
                            else {
                                VersionId v{};
                                v[0] = 111;
                                base = v;
                            }
                            r.type = 21;
                            r.payload =
                                encodePlanDone({session, base, 0, 1, 0, 0, 1, 3, "Unchanged"});
                        } else if (p.pageIndex == 0) {
                            r.type = 20;
                            r.payload = encodePlanPage({p.sessionId, std::nullopt, 0, {{0, 1}}});
                        } else {
                            r.type = 21;
                            r.payload = encodePlanDone(
                                {p.sessionId, std::nullopt, 1, 1, 1, 3, 0, 0, "Added"});
                        }
                    } else if (q.type == 5) {
                        auto p = decodeDataChunk(q.payload);
                        if (failBlock.exchange(false)) {
                            socket->abort();
                            break;
                        }
                        ++blocks;
                        r.type = 6;
                        r.payload =
                            encodeChunkAck({p.sessionId, p.offset,
                                            static_cast<uint32_t>(p.data.size()), p.sha256});
                    } else if (q.type == 28 || q.type == 30) {
                        auto p = decodeQueryDelta(q.payload);
                        auto key = QString::fromLatin1(
                            QByteArray(reinterpret_cast<char *>(p.sessionId.data()), 16).toHex());
                        auto &s = sessions.at(key);
                        VersionId version{};
                        version[0] = 99;
                        DeltaResult done{p.sessionId,
                                         version,
                                         s.begin.metadata.path,
                                         s.begin.metadata.size,
                                         s.begin.metadata.sha256,
                                         true};
                        if (q.type == 30) {
                            s.committed = true;
                            r.type = 31;
                            r.payload = encodeDeltaResult(done);
                        } else {
                            ++queries;
                            r.type = 29;
                            DeltaStatus status{
                                p.sessionId,
                                DeltaMode::Upload,
                                s.committed ? DeltaPhase::Committed : DeltaPhase::Transferring,
                                std::nullopt,
                                1,
                                s.committed ? "" : "01",
                                s.committed ? std::optional<DeltaResult>{done} : std::nullopt};
                            r.payload = encodeDeltaStatus(status);
                        }
                    } else
                        break;
                    auto wire = encode(r);
                    socket->write(wire.data(), static_cast<qint64>(wire.size()));
                    socket->waitForBytesWritten(100);
                }
            }
        });
        port = future.get();
        check(port != 0);
    }
    ~FakeServer() {
        stopping = true;
        if (thread.joinable())
            thread.join();
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir dir;
        gui::BackupConfig c;
        c.directory = dir.path() + "/missing";
        c.stateDirectory = dir.path() + "/state";
        gui::RunControl control;
        int errors = 0;
        gui::WorkerCallbacks cb;
        gui::TransferProgress lastProgress;
        cb.progress = [&](const auto &p) { lastProgress = p; };
        cb.log = [&](const QString &level, const QString &) {
            if (level == "错误")
                ++errors;
        };
        auto r = gui::runBackup(c, true, {}, control, cb);
        check(r.failed > 0 && errors > 0);
        c.directory = dir.path();
        control.cancel();
        r = gui::runBackup(c, true, {}, control, cb);
        check(r.cancelled && r.succeeded == 0);
        gui::RunControl other;
        c.chunkSize = 0;
        r = gui::runBackup(c, true, {}, other, cb);
        check(r.failed > 0);
        // 10000 valid files plus one excluded entry must fail explicitly before networking,
        // rather than silently drop a real preview row from the capped GUI model.
        c.directory = dir.path() + "/limit";
        c.stateDirectory = dir.path() + "/limit-state";
        check(QDir().mkpath(c.directory + "/build"));
        for (int n = 0; n < 10000; ++n) {
            QFile f(c.directory + "/" + QString::number(n));
            check(f.open(QIODevice::WriteOnly));
        }
        c.exclusions = {"build"};
        c.chunkSize = 262144;
        c.port = 1;
        QString lastError;
        cb.log = [&](const QString &, const QString &message) { lastError = message; };
        r = gui::runBackup(c, true, {}, other, cb);
        if (!(r.failed == 1 && r.succeeded == 0 && lastError.contains("10000")))
            std::cerr << "limit: failed=" << r.failed << " success=" << r.succeeded
                      << " error=" << lastError.toUtf8().constData() << '\n';
        check(r.failed == 1 && r.succeeded == 0 && lastError.contains("10000"));
        check(!r.planComplete);
        // Chinese source name and separate private state; one block is 3 bytes.
        QDir().mkpath(dir.path() + "/source");
        QFile source(dir.path() + "/source/中文.txt");
        check(source.open(QIODevice::WriteOnly));
        source.write("abc");
        source.close();
        FakeServer server;
        c.directory = dir.path() + "/source";
        c.chunkSize = 262144;
        c.port = server.port;
        c.mode = "full";
        {
            auto automatic = c;
            automatic.stateDirectory = dir.path() + "/auto-session";
            automatic.mode = "auto";
            automatic.resume = false;
            QList<gui::FileRecord> automaticRows;
            auto automaticCallbacks = cb;
            automaticCallbacks.entry = [&](const auto &e) { automaticRows.append(e); };
            const auto planned = gui::runBackup(automatic, true, {}, other, automaticCallbacks);
            if (planned.failed || planned.succeeded != 1 || automaticRows.last().advice != gui::BackupAdvice::Full)
                throw std::runtime_error("auto must plan a new file as full, independent of the manual resume switch");
        }
        {
            auto explicitResume = c;
            explicitResume.stateDirectory = dir.path() + "/no-session";
            explicitResume.mode = "resume";
            explicitResume.resume = false;
            const auto rejected = gui::runBackup(explicitResume, true, {}, other, cb);
            if (rejected.failed != 1 || rejected.succeeded != 0)
                throw std::runtime_error("explicit resume without a checkpoint must not silently preview a new upload");
        }
        QList<gui::FileRecord> pins;
        cb.entry = [&](const gui::FileRecord &e) { pins.append(e); };
        r = gui::runBackup(c, true, {}, other, cb);
        check(!r.failed && r.succeeded == 1 && pins.size() == 1 && pins[0].sha256.size() == 64 &&
              pins[0].missingBytes == 3 && server.blocks == 0);
        auto original = pins;
        server.failBlock = true;
        QList<gui::FileRecord> uploaded;
        cb.entry = [&](const auto &e) { uploaded.append(e); };
        r = gui::runBackup(c, false, original, other, cb);
        check(r.failed == 1 && server.blocks == 0);
        // Missing original sources cannot disappear from an unfinished batch.
        auto renamed = source.fileName() + ".renamed";
        check(QFile::rename(source.fileName(), renamed));
        r = gui::runBackup(c, true, {}, other, cb);
        check(r.failed > 0 && server.queries == 0);
        check(QFile::rename(renamed, source.fileName()));
        {
            auto automatic = c;
            automatic.mode = "auto";
            automatic.resume = false;
            QList<gui::FileRecord> autoPins;
            auto callbacks = cb;
            callbacks.entry = [&](const auto &e) { autoPins.append(e); };
            auto planned = gui::runBackup(automatic, true, {}, other, callbacks);
            check(!planned.failed && planned.planComplete && autoPins.last().strategy == gui::BackupStrategy::Resume);
            const auto pinned = autoPins;
            server.rejectPreviewBaseline = true;
            auto rejected = gui::runBackup(automatic, false, pinned, other, callbacks);
            check(rejected.failed == 1 && server.blocks == 0 &&
                  autoPins.last().strategy == gui::BackupStrategy::Error && autoPins.last().versionId.isEmpty());
        }
        c.mode = "resume";
        r = gui::runBackup(c, false, original, other, cb);
        check(r.failed == 0 && r.succeeded == 1 && server.blocks == 1 && server.queries >= 1 &&
              uploaded.last().advice == gui::BackupAdvice::Resume &&
              uploaded.last().versionId.size() == 32 && uploaded.last().change == "新增" &&
              uploaded.last().changedBlocks == 1 && lastProgress.totalBytes == 3 &&
              lastProgress.acknowledgedBytes == 3);
        r = gui::runBackup(c, false, original, other, cb);
        // This minimal peer reports no current version even after COMMITTED.
        // Explicit Resume must not silently start a replacement upload.
        check(r.failed == 1 && server.blocks == 1 && uploaded.last().versionId.isEmpty());
        const auto stateNames = QDir(c.stateDirectory).entryList({"*.json"}, QDir::Files);
        check(stateNames.size() == 1);
        QFile state(c.stateDirectory + "/" + stateNames.first());
        check(state.open(QIODevice::ReadOnly));
        auto saved = QJsonDocument::fromJson(state.readAll()).object();
        state.close();
        saved["completed"] = false;
        // Simulates an interrupted first/full upload selected via Resume on reopen.
        auto identity = saved["identity"].toObject();
        identity["mode"] = "full";
        saved["identity"] = identity;
        check(state.open(QIODevice::WriteOnly | QIODevice::Truncate));
        state.write(QJsonDocument(saved).toJson());
        state.close();
        c.mode = "incremental";
        uploaded.clear();
        r = gui::runBackup(c, true, {}, other, cb);
        check(!r.failed && uploaded.size() == 2 && !uploaded.first().versionId.isEmpty() &&
              uploaded.last().status == "就绪");
        check(state.open(QIODevice::ReadOnly));
        check(QJsonDocument::fromJson(state.readAll()).object()["completed"].toBool());
        state.close();
        const auto originalState = c.stateDirectory;
        for (int evil : {1, 2}) {
            c.stateDirectory = dir.path() + QString("/evil%1").arg(evil);
            server.evilPlan = evil;
            r = gui::runBackup(c, false, original, other, cb);
            check(r.failed == 1);
            auto names = QDir(c.stateDirectory).entryList({"*.json"}, QDir::Files);
            check(names.size() == 1);
            QFile pending(c.stateDirectory + "/" + names.first());
            check(pending.open(QIODevice::ReadOnly));
            auto receipt = QJsonDocument::fromJson(pending.readAll()).object();
            check(!receipt.contains("plan") && !receipt["completed"].toBool());
        }
        c.stateDirectory = originalState;
        check(source.open(QIODevice::WriteOnly));
        source.write("def");
        source.close();
        r = gui::runBackup(c, false, original, other, cb);
        check(r.failed == 1 && server.blocks == 1);
        server.wrongId = true;
        r = gui::runBackup(c, true, {}, other, cb);
        check(r.failed == 1 && server.blocks == 1);
        gui::RunControl paused;
        paused.paused = true;
        auto pending =
            std::async(std::launch::async, [&] { return gui::runBackup(c, true, {}, paused, cb); });
        check(pending.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
        paused.cancel();
        check(pending.get().cancelled);
        // A state path which is an ordinary file must stop before a request.
        c.stateDirectory = source.fileName();
        r = gui::runBackup(c, true, {}, other, cb);
        check(r.failed > 0);
        std::cout << "invalid config, cancellation, UTF8 preview, interrupted/resumed/committed "
                     "sessions, source change and wrong request ID passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
