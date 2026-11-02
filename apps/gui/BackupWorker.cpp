#include "TransferTypes.h"
#include "chunkharbor/client_core/DeltaExchange.h"
#include "chunkharbor/client_core/DirectoryScanner.h"
#include "chunkharbor/client_core/HandshakeExchange.h"
#include "chunkharbor/protocol/ChunkAck.h"
#include "chunkharbor/protocol/UploadControl.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTcpSocket>
#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
namespace {
using namespace chunkharbor::protocol;
using namespace chunkharbor::client;
std::string narrow(const QByteArray &b) { return {b.constData(), static_cast<size_t>(b.size())}; }
std::string utf8(const QString &s) { return narrow(s.toUtf8()); }
QString qstr(const std::string &s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}
QString friendly(const std::exception &e) {
    const auto detail = qstr(e.what());
    const auto key = detail.toLower();
    QString message;
    if (key.contains("checkpoint") || key.contains("state directory"))
        message = "无法读取或保存续传记录，请检查状态目录权限，并保持原任务的源文件和配置一致。";
    else if (key.contains("source") || key.contains("directory changed"))
        message = "源文件或目录已变化，请恢复原文件以续传，或重新扫描后创建备份。";
    else if (key.contains("timeout") || key.contains("timed out") || key.contains("connection") ||
             key.contains("disconnected") || key.contains("socket"))
        message = "连接中断或服务器未响应，请检查SSH隧道和服务端后重试；已确认进度会保留。";
    else if (key.contains("base") || key.contains("conflict") || key.contains("exists"))
        message = "远端版本与预览不一致，或目标已存在，请重新扫描；完整上传仅用于首次备份。";
    else if (key.contains("capability"))
        message = "服务器未提供增量上传能力，请以upload模式启动服务端。";
    else if (key.contains("configuration") || key.contains("geometry"))
        message = "备份参数无效，请检查分块大小、端口和备份模式。";
    else
        message = "备份未完成，请检查服务器响应和任务配置后重试。";
    return message + "（" + detail + "）";
}
void restorePlan(gui::FileRecord &r, const QJsonObject &saved) {
    auto p = saved.value("plan").toObject();
    r.baseVersion = saved.value("base").toString();
    if (p.isEmpty()) {
        r.change = "未知";
        r.note = "服务端已提交；原始差异统计不可用";
        return;
    }
    bool okBytes = false, okBlocks = false;
    auto bytes = p.value("missingBytes").toString().toULongLong(&okBytes);
    auto blocks = p.value("missingBlocks").toString().toULongLong(&okBlocks);
    auto kind = p.value("changeKind").toString();
    if (!okBytes || !okBlocks || blocks > 65536 || bytes > r.size ||
        (kind != "Added" && kind != "Modified" && kind != "Unchanged"))
        throw std::runtime_error("invalid saved plan statistics");
    r.missingBytes = bytes;
    r.changedBlocks = blocks;
    r.change = kind == "Added" ? "新增" : kind == "Unchanged" ? "未变化" : "已修改";
}
std::filesystem::path fsPath(const QString &s) { return std::filesystem::u8path(utf8(s)); }
void cancelled(gui::RunControl &c) {
    if (c.checkpoint())
        throw std::runtime_error("cancelled");
}
QString hex(const SessionId &id) {
    return QByteArray(reinterpret_cast<const char *>(id.data()), 16).toHex();
}
SessionId id(const QString &s) {
    if (s.size() != 32 || s.toLatin1() != QByteArray::fromHex(s.toLatin1()).toHex())
        throw std::runtime_error("invalid checkpoint ID");
    auto b = QByteArray::fromHex(s.toLatin1());
    SessionId result{};
    std::copy(b.begin(), b.end(), result.begin());
    return result;
}
void atomicJson(const QString &path, const QJsonObject &o) {
    QSaveFile file(path);
    auto bytes = QJsonDocument(o).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("checkpoint publication failed");
}
QJsonObject readJson(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16384)
        throw std::runtime_error("invalid checkpoint file");
    QJsonParseError e;
    auto d = QJsonDocument::fromJson(file.readAll(), &e);
    if (e.error != QJsonParseError::NoError || !d.isObject())
        throw std::runtime_error("invalid checkpoint JSON");
    return d.object();
}
class Peer {
    QTcpSocket socket;
    std::string input;
    gui::RunControl &control;

  public:
    Peer(quint16 port, gui::RunControl &c) : control(c) {
        socket.setReadBufferSize(HeaderSize + MaxControlPayload + 1);
        socket.connectToHost(QHostAddress::LocalHost, port);
        QElapsedTimer clock;
        clock.start();
        while (socket.state() != QAbstractSocket::ConnectedState) {
            cancelled(control);
            if (clock.elapsed() > 5000 || socket.state() == QAbstractSocket::UnconnectedState)
                throw std::runtime_error("connection failed");
            socket.waitForConnected(100);
        }
        Hello h;
        h.capabilities = {"handshake", "upload", "fixed_block_delta_v1"};
        HandshakeExchange x(h, 1);
        x.accept(request(x.request()));
        if (std::find(x.ack().capabilities.begin(), x.ack().capabilities.end(),
                      "fixed_block_delta_v1") == x.ack().capabilities.end())
            throw std::runtime_error("server lacks incremental upload capability");
    }
    Frame request(const Frame &f) {
        cancelled(control);
        auto bytes = encode(f);
        QElapsedTimer clock;
        clock.start();
        if (socket.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
            static_cast<qint64>(bytes.size()))
            throw std::runtime_error("socket write failed");
        while (socket.bytesToWrite()) {
            if (control.cancelled)
                throw std::runtime_error("cancelled");
            if (clock.elapsed() > 30000 || socket.state() != QAbstractSocket::ConnectedState)
                throw std::runtime_error("socket write timeout/disconnected");
            socket.waitForBytesWritten(100);
        }
        for (;;) {
            if (control.cancelled)
                throw std::runtime_error("cancelled");
            auto decoded = decode(input);
            if (decoded.status == DecodeStatus::Error)
                throw std::runtime_error("invalid response frame");
            if (decoded.status == DecodeStatus::Complete) {
                if (decoded.consumed != input.size() || socket.bytesAvailable())
                    throw std::runtime_error("unexpected trailing response data");
                input.clear();
                if (decoded.frame.requestId != f.requestId)
                    throw std::runtime_error("response ID mismatch");
                if (decoded.frame.type == 15) {
                    auto e = decodeUploadError(decoded.frame.payload);
                    throw std::runtime_error(e.code + ": " + e.message);
                }
                return decoded.frame;
            }
            if (input.size() >= HeaderSize + MaxControlPayload)
                throw std::runtime_error("response exceeds control limit");
            if (input.size() >= HeaderSize) {
                std::uint64_t length = 0;
                for (int i = 20; i < 28; ++i)
                    length = (length << 8) | static_cast<unsigned char>(input[i]);
                if (length > MaxControlPayload)
                    throw std::runtime_error("response payload exceeds limit");
            }
            if (clock.elapsed() > 30000)
                throw std::runtime_error("server response timed out");
            if (!socket.bytesAvailable()) {
                if (socket.state() != QAbstractSocket::ConnectedState)
                    throw std::runtime_error("server disconnected");
                socket.waitForReadyRead(100);
                continue;
            }
            input += narrow(
                socket.read(static_cast<qint64>(HeaderSize + MaxControlPayload - input.size())));
        }
    }
};
#ifdef Q_OS_WIN
struct NativeHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit NativeHandle(HANDLE handle) : value(handle) {
        if (value == INVALID_HANDLE_VALUE)
            throw std::runtime_error("source native open failed");
    }
    ~NativeHandle() { CloseHandle(value); }
    NativeHandle(const NativeHandle &) = delete;
};
struct Owned {
    std::vector<std::shared_ptr<NativeHandle>> parents;
    std::shared_ptr<NativeHandle> file;
    gui::RunControl &control;
    std::uint64_t size = 0;
    Owned(const std::filesystem::path &path, gui::RunControl &control) : control(control) {
        auto absolute = std::filesystem::absolute(path).lexically_normal();
        auto checked = absolute.root_path();
        auto relative = absolute.relative_path();
        for (auto it = relative.begin(); it != relative.end(); ++it) {
            checked /= *it;
            auto next = it;
            ++next;
            bool directory = next != relative.end();
            auto handle = std::make_shared<NativeHandle>(CreateFileW(
                checked.c_str(), directory ? FILE_READ_ATTRIBUTES : GENERIC_READ,
                directory ? (FILE_SHARE_READ | FILE_SHARE_WRITE) : FILE_SHARE_READ, nullptr,
                OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0),
                nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle->value, &info) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory)
                throw std::runtime_error("source reparse/type rejected");
            if (directory)
                parents.push_back(handle);
            else {
                file = handle;
                size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            }
        }
        if (!file || size > 64ULL * 1024 * 1024)
            throw std::runtime_error("invalid source or source exceeds 64 MiB");
    }
    std::string read(std::uint64_t offset, std::size_t count) {
        if (control.checkpoint())
            throw std::runtime_error("cancelled");
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file->value, position, nullptr, FILE_BEGIN))
            throw std::runtime_error("source seek failed");
        std::string bytes(count, '\0');
        DWORD got = 0;
        if (!ReadFile(file->value, bytes.data(), static_cast<DWORD>(count), &got, nullptr) ||
            got != count)
            throw std::runtime_error("source short read");
        return bytes;
    }
    std::string digest() {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (std::uint64_t offset = 0; offset < size;) {
            auto bytes = read(
                offset, static_cast<std::size_t>(std::min<std::uint64_t>(65536, size - offset)));
            hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
            offset += bytes.size();
        }
        return narrow(hash.result().toHex());
    }
};

#else
struct Owned {
    QFile file;
    gui::RunControl &control;
    quint64 size = 0;
    Owned(const std::filesystem::path &path, gui::RunControl &c)
        : file(QString::fromStdString(path.u8string())), control(c) {
        auto absolute = QFileInfo(file).absoluteFilePath();
        auto p = QFileInfo(absolute);
        for (;;) {
            if (p.isSymLink())
                throw std::runtime_error("source link rejected");
            auto parent = p.dir().absolutePath();
            if (parent == p.absoluteFilePath())
                break;
            p = QFileInfo(parent);
        }
        if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly))
            throw std::runtime_error("cannot open source");
        size = static_cast<quint64>(file.size());
        if (size > 64ULL * 1024 * 1024)
            throw std::runtime_error("source exceeds 64 MiB");
    }
    std::string read(quint64 offset, size_t count) {
        cancelled(control);
        if (!file.seek(static_cast<qint64>(offset)))
            throw std::runtime_error("source seek failed");
        auto b = file.read(static_cast<qint64>(count));
        if (b.size() != static_cast<qsizetype>(count))
            throw std::runtime_error("source short read");
        return narrow(b);
    }
    std::string digest() {
        if (static_cast<quint64>(file.size()) != size)
            throw std::runtime_error("source size changed");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (quint64 o = 0; o < size;) {
            auto b = read(o, static_cast<size_t>(std::min<quint64>(65536, size - o)));
            hash.addData(QByteArrayView(b.data(), static_cast<qsizetype>(b.size())));
            o += b.size();
        }
        return narrow(hash.result().toHex());
    }
};
#endif
SourceSnapshot snapshot(const std::shared_ptr<Owned> &o, const QString &logical, quint32 chunk) {
    auto digest = o->digest();
    BeginUpload m{utf8(logical), o->size, 0, chunk, digest};
    (void)encodeBeginUpload(m);
    return {m, [o](auto offset, auto count) { return o->read(offset, count); },
            [](std::string_view b) {
                auto h = QCryptographicHash::hash(
                    QByteArrayView(b.data(), static_cast<qsizetype>(b.size())),
                    QCryptographicHash::Sha256);
                Digest d{};
                std::copy(h.begin(), h.end(), d.begin());
                return d;
            },
            [o, digest] {
                if (o->digest() != digest)
                    throw std::runtime_error("source changed after preview");
            }};
}
} // namespace
namespace gui {
RunResult runBackup(const BackupConfig &config, bool preview, const QList<FileRecord> &pinned,
                    RunControl &control, const WorkerCallbacks &cb) {
    RunResult result;
    TransferProgress progress;
    QElapsedTimer timer;
    timer.start();
    qint64 lastProgress = -100;
    auto emitProgress = [&](const QString &phase, bool force = false) {
        progress.phase = phase;
        progress.bytesPerSecond =
            timer.elapsed() > 0 ? progress.acknowledgedBytes * 1000.0 / timer.elapsed() : 0;
        if (cb.progress && (force || timer.elapsed() - lastProgress >= 100)) {
            lastProgress = timer.elapsed();
            cb.progress(progress);
        }
    };
    auto log = [&](const QString &level, const QString &message) {
        if (cb.log)
            cb.log(level, message);
    };
    std::set<QString> deliveredPaths;
    auto entry = [&](const FileRecord &r) {
        if (deliveredPaths.count(r.logicalPath) == 0 && deliveredPaths.size() >= 10000)
            throw std::runtime_error(
                "directory entry limit exceeded (10000 including excluded/error entries)");
        deliveredPaths.insert(r.logicalPath);
        if (cb.entry)
            cb.entry(r);
    };
    try {
        cancelled(control);
        if (!config.port || !config.chunkSize || config.chunkSize > MaxChunkDataSize ||
            (config.mode != "full" && config.mode != "resume" && config.mode != "incremental"))
            throw std::runtime_error("invalid task configuration");
        if (config.stateDirectory.isEmpty())
            throw std::runtime_error("state directory required");
        auto stateDir = QFileInfo(config.stateDirectory).absoluteFilePath();
        auto root = QFileInfo(config.directory).absoluteFilePath();
        if (root == stateDir)
            throw std::runtime_error("state directory cannot equal source root");
        if (!QDir().mkpath(stateDir))
            throw std::runtime_error("cannot create state directory");
        std::set<std::string> exclusions;
        for (const auto &e : config.exclusions)
            exclusions.insert(utf8(e));
        DirectoryScanner scanner(fsPath(root), exclusions, fsPath(stateDir));
        QList<FileRecord> records;
        std::set<QString> scannedPaths;
        emitProgress("正在扫描", true);
        while (auto e = scanner.next()) {
            cancelled(control);
            FileRecord r;
            r.localPath = QFileInfo(qstr(e->localPath.u8string())).absoluteFilePath();
            r.logicalPath = config.prefix.isEmpty() ? qstr(e->logicalPath)
                                                    : config.prefix + "/" + qstr(e->logicalPath);
            if (scannedPaths.size() >= 10000)
                throw std::runtime_error(
                    "directory entry limit exceeded (10000 including excluded/error entries)");
            scannedPaths.insert(r.logicalPath);
            r.endpoint = QString("127.0.0.1:%1").arg(config.port);
            if (e->excluded) {
                r.status = "已跳过";
                r.change = "已排除";
                r.note = "目录排除规则";
                entry(r);
                ++result.skipped;
                continue;
            }
            if (!e->error.empty()) {
                r.versionId.clear();
                r.status = "错误";
                r.change = "错误";
                r.note = qstr(e->error);
                entry(r);
                ++result.failed;
                continue;
            }
            r.size = static_cast<quint64>(QFileInfo(r.localPath).size());
            records.append(r);
        }
        // An unfinished task must not silently forget a source deleted between runs.
        if (config.resume) {
            const auto states = QDir(stateDir).entryList({"*.json"}, QDir::Files);
            if (states.size() > 20001)
                throw std::runtime_error("checkpoint record limit exceeded");
            for (const auto &name : states) {
                auto saved = readJson(stateDir + "/" + name);
                auto identity = saved.value("identity").toObject();
                if (identity.value("root").toString() != root ||
                    identity.value("endpoint").toString() !=
                        QString("127.0.0.1:%1").arg(config.port) ||
                    saved.value("completed").toBool())
                    continue;
                bool found = false;
                for (const auto &r : records)
                    if (r.localPath == identity.value("local").toString() &&
                        r.logicalPath == identity.value("path").toString()) {
                        found = true;
                        break;
                    }
                if (!found)
                    throw std::runtime_error("unfinished backup source missing or excluded; "
                                             "restore source/configuration");
            }
        }
        progress.totalFiles = records.size();
        std::map<QString, FileRecord> pins;
        for (const auto &p : pinned)
            if (p.status != "已跳过" && p.status != "错误")
                pins.emplace(p.logicalPath, p);
        if (!preview && pins.size() != static_cast<size_t>(records.size()))
            throw std::runtime_error("directory changed or preview required; scan again");
        if (!preview)
            for (const auto &r : records) {
                auto p = pins.find(r.logicalPath);
                if (p == pins.end())
                    throw std::runtime_error("directory changed after preview");
                progress.totalBytes += p->second.missingBytes;
            }
        for (auto r : records) {
            cancelled(control);
            progress.file = r.logicalPath;
            emitProgress("正在生成分块清单", true);
            try {
                auto owned = std::make_shared<Owned>(fsPath(r.localPath), control);
                auto source = snapshot(owned, r.logicalPath, config.chunkSize);
                r.sha256 = qstr(source.metadata.sha256);
                r.size = source.metadata.size;
                DeltaOptions options;
                options.mode = preview ? DeltaMode::Preview : DeltaMode::Upload;
                options.cancelled = [&] { return control.checkpoint(); };
                if (config.mode == "full")
                    options.base.kind = BaseSelector::Kind::Absent;
                if (!preview) {
                    const auto &p = pins.at(r.logicalPath);
                    if (p.sha256 != r.sha256 || p.size != r.size || p.localPath != r.localPath)
                        throw std::runtime_error("source changed after preview; scan again");
                    r.baseVersion = p.baseVersion;
                    if (p.baseVersion == "absent")
                        options.base.kind = BaseSelector::Kind::Absent;
                    else {
                        options.base.kind = BaseSelector::Kind::Exact;
                        options.base.versionId = id(p.baseVersion);
                    }
                }
                QJsonObject identity{{"root", root},
                                     {"local", QFileInfo(r.localPath).absoluteFilePath()},
                                     {"path", r.logicalPath},
                                     {"endpoint", r.endpoint},
                                     {"chunk", QString::number(config.chunkSize)},
                                     {"size", QString::number(r.size)},
                                     {"sha256", r.sha256},
                                     {"mode", config.mode == "full" ? "full" : "incremental"},
                                     {"base", preview ? QString() : r.baseVersion}};
                auto key = QCryptographicHash::hash((root + "\n" + r.logicalPath + "\n" +
                                                     r.endpoint + (preview ? "preview" : "upload"))
                                                        .toUtf8(),
                                                    QCryptographicHash::Sha256)
                               .toHex();
                auto statePath = stateDir + "/" + QString::fromLatin1(key) + ".json";
                auto uploadKey =
                    QCryptographicHash::hash(
                        (root + "\n" + r.logicalPath + "\n" + r.endpoint + "upload").toUtf8(),
                        QCryptographicHash::Sha256)
                        .toHex();
                auto uploadPath = stateDir + "/" + QString::fromLatin1(uploadKey) + ".json";
                QJsonObject saved;
                if (config.resume && QFileInfo::exists(uploadPath)) {
                    saved = readJson(uploadPath);
                    auto previous = saved.value("identity").toObject();
                    if (config.mode == "resume" && previous.value("mode").toString() == "full")
                        identity["mode"] = "full";
                    auto previousSource = previous, currentSource = identity;
                    previousSource.remove("base");
                    currentSource.remove("base");
                    if (!saved.value("completed").toBool() && (preview || previous != identity)) {
                        if (previousSource != currentSource)
                            throw std::runtime_error("pending checkpoint source/configuration "
                                                     "mismatch; restore original source");
                        DeltaCheckpoint checkpoint{
                            id(saved.value("session").toString()),
                            saved.value("base").toString() == "absent"
                                ? std::optional<VersionId>{}
                                : std::optional<VersionId>{id(saved.value("base").toString())}};
                        auto response = [&] {
                            Peer queryPeer(config.port, control);
                            return queryPeer.request(
                                {28, 2, encodeQueryDelta({checkpoint.session})});
                        }();
                        if (response.type != 29)
                            throw std::runtime_error("unexpected session status response type");
                        auto status = decodeDeltaStatus(response.payload);
                        if (status.sessionId != checkpoint.session ||
                            status.mode != DeltaMode::Upload ||
                            status.baseVersion != checkpoint.base)
                            throw std::runtime_error("saved session query identity mismatch");
                        if (status.phase == DeltaPhase::Committed) {
                            // The server may commit before the client can publish its local
                            // receipt. Reuse the core's complete result verification before
                            // acknowledging locally.
                            DeltaOptions recovery;
                            recovery.mode = DeltaMode::Upload;
                            recovery.resume = checkpoint;
                            recovery.cancelled = [&] { return control.checkpoint(); };
                            recovery.base.kind = checkpoint.base ? BaseSelector::Kind::Exact
                                                                 : BaseSelector::Kind::Absent;
                            if (checkpoint.base)
                                recovery.base.versionId = *checkpoint.base;
                            Peer recoveryPeer(config.port, control);
                            auto recovered = runDelta(
                                source, [&](const Frame &f) { return recoveryPeer.request(f); },
                                recovery);
                            if (!recovered.result)
                                throw std::runtime_error("missing recovered commit result");
                            auto receipt = r;
                            receipt.status = "已完成";
                            receipt.versionId = hex(recovered.result->versionId);
                            receipt.baseVersion = saved.value("base").toString();
                            receipt.time =
                                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                            restorePlan(receipt, saved);
                            receipt.note = receipt.change == "未知"
                                               ? "已核验提交；原始差异统计不可用"
                                               : "已核验并恢复服务器提交记录";
                            saved["completed"] = true;
                            atomicJson(uploadPath, saved);
                            entry(receipt);
                            log("信息", r.logicalPath + "：" + receipt.note);
                        }
                    }
                    if (!preview) {
                        if (previous != identity) {
                            if (saved.value("completed").toBool())
                                saved = {};
                            else
                                throw std::runtime_error("pending checkpoint baseline mismatch; "
                                                         "retry the original preview");
                        } else
                            options.resume = DeltaCheckpoint{
                                id(saved.value("session").toString()),
                                saved.value("base").toString() == "absent"
                                    ? std::optional<VersionId>{}
                                    : std::optional<VersionId>{id(saved.value("base").toString())}};
                    }
                }
                options.save = [&](const DeltaCheckpoint &cp) {
                    if (preview)
                        return;
                    saved = {{"identity", identity},
                             {"session", hex(cp.session)},
                             {"base", cp.base ? hex(*cp.base) : "absent"},
                             {"completed", false}};
                    atomicJson(statePath, saved);
                };
                quint64 expectedBytes = preview ? 0 : pins.at(r.logicalPath).missingBytes;
                auto adjustTotal = [&](quint64 actual) {
                    if (preview)
                        return;
                    progress.totalBytes = progress.totalBytes - expectedBytes + actual;
                    expectedBytes = actual;
                };
                Peer peer(config.port, control);
                std::optional<PlanValidator> historyPlan;
                auto outcome = runDelta(
                    source,
                    [&](const Frame &f) {
                        const QString phase = f.type == 18   ? "正在生成分块清单"
                                              : f.type == 27 ? "正在请求上传计划"
                                              : f.type == 5  ? "正在上传变化块"
                                              : f.type == 30 ? "正在校验并提交"
                                                             : "正在请求上传计划";
                        emitProgress(phase, f.type == 30);
                        auto reply = peer.request(f);
                        if (!preview && reply.type == 20) {
                            auto p = decodePlanPage(reply.payload);
                            if (!historyPlan)
                                historyPlan.emplace(id(saved.value("session").toString()),
                                                    saved.value("base").toString() == "absent"
                                                        ? std::optional<VersionId>{}
                                                        : std::optional<VersionId>{id(
                                                              saved.value("base").toString())},
                                                    r.size, config.chunkSize);
                            historyPlan->append(p);
                        }
                        if (!preview && reply.type == 21) {
                            auto p = decodePlanDone(reply.payload);
                            if (!historyPlan)
                                historyPlan.emplace(id(saved.value("session").toString()),
                                                    saved.value("base").toString() == "absent"
                                                        ? std::optional<VersionId>{}
                                                        : std::optional<VersionId>{id(
                                                              saved.value("base").toString())},
                                                    r.size, config.chunkSize);
                            historyPlan->finish(p);
                            saved["plan"] =
                                QJsonObject{{"missingBytes", QString::number(p.missingBytes)},
                                            {"missingBlocks", QString::number(p.missingBlocks)},
                                            {"changeKind", qstr(p.changeKind)}};
                            atomicJson(statePath, saved);
                            adjustTotal(p.missingBytes);
                        }
                        if (!preview && f.type == 28 && reply.type == 29) {
                            auto status = decodeDeltaStatus(reply.payload);
                            if (status.phase == DeltaPhase::Committed)
                                adjustTotal(0);
                            else if (status.phase == DeltaPhase::Transferring ||
                                     status.phase == DeltaPhase::Verifying) {
                                auto bits = QByteArray::fromHex(
                                    QByteArray::fromStdString(status.missingBitmap));
                                quint64 remaining = 0;
                                quint64 blocks = (r.size + config.chunkSize - 1) / config.chunkSize;
                                if (static_cast<quint64>(bits.size()) != (blocks + 7) / 8)
                                    throw std::runtime_error("invalid resume bitmap");
                                for (quint64 i = 0; i < blocks; ++i)
                                    if (static_cast<unsigned char>(
                                            bits[static_cast<qsizetype>(i / 8)]) &
                                        (1U << (i % 8)))
                                        remaining += std::min<quint64>(
                                            config.chunkSize, r.size - i * config.chunkSize);
                                adjustTotal(remaining);
                            }
                        }
                        if (f.type == 5 && reply.type == 6) {
                            auto chunk = decodeDataChunk(f.payload);
                            auto ack = decodeChunkAck(reply.payload);
                            validateChunkAckForChunk(ack, chunk);
                            progress.acknowledgedBytes += chunk.data.size();
                            emitProgress(phase);
                        }
                        return reply;
                    },
                    options);
                r.baseVersion =
                    outcome.plan.baseVersion ? hex(*outcome.plan.baseVersion) : "absent";
                r.missingBytes = outcome.plan.missingBytes;
                r.changedBlocks = outcome.plan.missingBlocks;
                r.change = outcome.plan.changeKind == "Added"       ? "新增"
                           : outcome.plan.changeKind == "Unchanged" ? "未变化"
                                                                    : "已修改";
                r.status = preview ? "就绪" : "已完成";
                r.note = preview
                             ? QString("变化 %1 块，预计上传 %2 字节")
                                   .arg(r.changedBlocks)
                                   .arg(r.missingBytes)
                             : QString("服务端已确认提交，本轮发送 %1 块").arg(outcome.sentBlocks);
                if (preview)
                    progress.totalBytes += r.missingBytes;
                else {
                    if (!outcome.result)
                        throw std::runtime_error("server did not commit");
                    if (options.resume || saved.contains("plan")) {
                        const auto note = r.note;
                        restorePlan(r, saved);
                        if (r.change != "未知")
                            r.note = note;
                    }
                    r.versionId = hex(outcome.result->versionId);
                    r.time = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                    saved["completed"] = true;
                    atomicJson(statePath, saved);
                }
                ++result.succeeded;
                ++progress.completedFiles;
                entry(r);
                log("信息", r.logicalPath + "：" + r.note);
                emitProgress(preview ? "预览完成" : "提交完成", true);
            } catch (const std::exception &e) {
                if (control.cancelled)
                    throw;
                r.versionId.clear();
                r.status = "错误";
                r.change = "错误";
                r.note = friendly(e);
                entry(r);
                log("错误", r.logicalPath + "：" + r.note);
                ++result.failed;
            }
        }
    } catch (const std::exception &e) {
        if (control.cancelled) {
            result.cancelled = true;
            log("警告", "任务已取消，已确认进度保留，可重试续传。");
        } else {
            ++result.failed;
            log("错误", friendly(e));
        }
    }
    result.cancelled = result.cancelled || control.cancelled.load();
    emitProgress(result.cancelled ? "已取消"
                 : result.failed  ? "部分失败"
                 : preview        ? "预览完成"
                                  : "提交完成",
                 true);
    return result;
}
} // namespace gui
