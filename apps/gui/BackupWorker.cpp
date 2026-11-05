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
#include <QNetworkProxy>
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
QString friendly(const std::exception &e, bool direct) {
    const auto detail = qstr(e.what());
    const auto key = detail.toLower();
    QString message;
    if (key.contains("no valid resume session"))
        message = "没有有效且匹配的续传会话；请按预览建议选择首次上传或增量备份并重新扫描。";
    else if (key.contains("session_not_found") || key.contains("session not resumable"))
        message = "续传会话失效或尚不可恢复；已保留检查点，请核验服务器后重试，或使用新的远端路径。";
    else if (key.contains("pending checkpoint source/configuration mismatch"))
        message = "该目标有未完成上传，但源文件或配置已变化，不能直接续传；请恢复原文件和配置，或使用新的远端路径备份。";
    else if (key.contains("full upload target changed"))
        message = "本任务已完成的文件在服务器上发生变化；首次完整上传不会覆盖，请选择增量备份或新的远端路径。";
    else if (key.contains("checkpoint") || key.contains("state directory"))
        message = "无法读取或保存续传记录，请检查状态目录权限，并保持原任务的源文件和配置一致。";
    else if (key.contains("source") || key.contains("directory changed"))
        message = "源文件或目录已变化，请恢复原文件以续传，或重新扫描后创建备份。";
    else if (key.contains("timeout") || key.contains("timed out") || key.contains("connection") ||
             key.contains("disconnected") || key.contains("socket"))
        message = direct ? "直接 TCP 连接失败或中断，请检查服务器地址、监听端口与网络；已确认进度会保留。"
                         : "SSH 转发端点连接失败或中断，请检查隧道与服务端；已确认进度会保留。";
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
    Peer(const gui::BackupConfig &config, gui::RunControl &c) : control(c) {
        socket.setProxy(QNetworkProxy::NoProxy);
        socket.setReadBufferSize(HeaderSize + MaxControlPayload + 1);
        socket.connectToHost(config.host, config.port, QIODevice::ReadWrite, QAbstractSocket::IPv4Protocol);
        QElapsedTimer clock;
        clock.start();
        while (socket.state() != QAbstractSocket::ConnectedState) {
            cancelled(control);
            if (clock.elapsed() > 5000 || socket.state() == QAbstractSocket::UnconnectedState)
                throw std::runtime_error(utf8(QString("connection failed %1:%2: %3")
                    .arg(config.host).arg(config.port).arg(socket.errorString())));
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
    const auto endpoint = QString("%1%2:%3").arg(config.direct ? "tcp:" : "", config.host).arg(config.port);
    RunResult result;
    const bool automatic = config.mode == "auto";
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
        if (config.host.isEmpty() || !config.port || !config.chunkSize || config.chunkSize > MaxChunkDataSize ||
            (!automatic && config.mode != "full" && config.mode != "resume" && config.mode != "incremental"))
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
            r.endpoint = endpoint;
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
                r.modeAllowed = false;
                if (automatic) {
                    records.append(r);
                    continue;
                }
                entry(r);
                ++result.failed;
                continue;
            }
            r.size = static_cast<quint64>(QFileInfo(r.localPath).size());
            records.append(r);
        }
        // An unfinished task must not silently forget a source deleted between runs.
        { // Retain safety checks even when automatic resume is disabled.
            std::set<QString> currentUploads;
            if (automatic)
                for (const auto &r : records)
                    currentUploads.insert(QString::fromLatin1(QCryptographicHash::hash(
                        (root + "\n" + r.logicalPath + "\n" + r.endpoint + "upload").toUtf8(),
                        QCryptographicHash::Sha256).toHex()) + ".json");
            const auto states = QDir(stateDir).entryList({"*.json"}, QDir::Files);
            if (states.size() > 20001)
                throw std::runtime_error("checkpoint record limit exceeded");
            for (const auto &name : states) {
                // Present files validate their checkpoints in an isolated try/catch.
                // Unassociated records still receive the conservative preflight.
                if (currentUploads.count(name)) continue;
                auto saved = readJson(stateDir + "/" + name);
                auto identity = saved.value("identity").toObject();
                if (identity.value("root").toString() != root ||
                    identity.value("endpoint").toString() !=
                        endpoint ||
                    saved.value("completed").toBool())
                    continue;
                const auto relative = QDir(root).relativeFilePath(identity.value("local").toString());
                const auto expectedPath = config.prefix.isEmpty() ? relative : config.prefix + "/" + relative;
                if (identity.value("path").toString() != expectedPath)
                    continue; // A different destination prefix is a separate backup task.
                bool found = false;
                for (const auto &r : records)
                    if (r.localPath == identity.value("local").toString() &&
                        r.logicalPath == identity.value("path").toString()) {
                        found = true;
                        break;
                    }
                if (!found) {
                    if (!automatic)
                        throw std::runtime_error("unfinished backup source missing or excluded; "
                                                 "restore source/configuration");
                    FileRecord missing;
                    missing.localPath = identity.value("local").toString();
                    missing.logicalPath = expectedPath;
                    missing.endpoint = identity.value("endpoint").toString();
                    missing.status = missing.change = "错误";
                    missing.modeAllowed = false;
                    missing.note = "未完成会话的源文件缺失或被排除；请恢复原源文件/配置，检查点保留。";
                    records.append(missing);
                }
            }
        }
        progress.totalFiles = records.size();
        std::map<QString, FileRecord> pins;
        for (const auto &p : pinned)
            if (p.change != "已排除" && (automatic || (p.status != "已跳过" && p.status != "错误")))
                pins.emplace(p.logicalPath, p);
        if (!preview && pins.size() != static_cast<size_t>(records.size()))
            throw std::runtime_error("directory changed or preview required; scan again");
        if (!preview)
            for (const auto &r : records) {
                auto p = pins.find(r.logicalPath);
                if (p == pins.end())
                    throw std::runtime_error("directory changed after preview");
                if (p->second.modeAllowed) progress.totalBytes += p->second.missingBytes;
            }
        for (auto r : records) {
            cancelled(control);
            progress.file = r.logicalPath;
            emitProgress("正在生成分块清单", true);
            try {
                if (automatic && !preview && pins.at(r.logicalPath).strategy == BackupStrategy::Error) {
                    r = pins.at(r.logicalPath);
                    r.note += "；本次未授权上传，请修复后重新扫描。";
                }
                if (automatic && (r.status == "错误" || (!preview && !r.modeAllowed))) {
                    r.modeAllowed = false;
                    r.strategy = BackupStrategy::Error;
                    entry(r);
                    ++result.failed;
                    ++progress.completedFiles;
                    continue;
                }
                auto owned = std::make_shared<Owned>(fsPath(r.localPath), control);
                auto source = snapshot(owned, r.logicalPath, config.chunkSize);
                r.sha256 = qstr(source.metadata.sha256);
                r.size = source.metadata.size;
                DeltaOptions options;
                options.mode = preview ? DeltaMode::Preview : DeltaMode::Upload;
                options.cancelled = [&] { return control.checkpoint(); };
                // Preview discovers actual state even for an unsuitable selected mode.
                // Execution is still bound to the preview's explicit baseline below.
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
                bool ownCompletedFull = false, pendingSession = false, committedSession = false;
                QString savedMode;
                std::optional<DeltaCheckpoint> checkpoint;
                std::optional<DeltaStatus> activeStatus;
                if (QFileInfo::exists(uploadPath)) {
                    saved = readJson(uploadPath);
                    if (!saved.value("identity").isObject() || !saved.value("completed").isBool())
                        throw std::runtime_error("invalid checkpoint fields");
                    auto previous = saved.value("identity").toObject();
                    savedMode = previous.value("mode").toString();
                    if (savedMode != "full" && savedMode != "incremental")
                        throw std::runtime_error("invalid checkpoint mode");
                    auto previousSource = previous, currentSource = identity;
                    for (auto *object : {&previousSource, &currentSource}) {
                        object->remove("base");
                        object->remove("mode");
                    }
                    const bool sourceMatches = previousSource == currentSource;
                    if (!saved.value("completed").toBool() && !sourceMatches)
                        throw std::runtime_error("pending checkpoint source/configuration mismatch; restore original source");
                    ownCompletedFull = config.mode == "full" && savedMode == "full" &&
                                       sourceMatches && saved.value("completed").toBool();
                    if (sourceMatches && (!saved.value("completed").toBool() || config.mode == "resume")) {
                        checkpoint = DeltaCheckpoint{id(saved.value("session").toString()),
                            saved.value("base").toString() == "absent" ? std::optional<VersionId>{}
                                : std::optional<VersionId>{id(saved.value("base").toString())}};
                        const auto response = [&] {
                            Peer queryPeer(config, control);
                            return queryPeer.request({28, 2, encodeQueryDelta({checkpoint->session})});
                        }();
                        if (response.type != 29)
                            throw std::runtime_error("unexpected session status response type");
                        const auto status = decodeDeltaStatus(response.payload);
                        if (status.sessionId != checkpoint->session || status.mode != DeltaMode::Upload ||
                            status.baseVersion != checkpoint->base)
                            throw std::runtime_error("saved session query identity mismatch");
                        if (status.phase == DeltaPhase::Committed) {
                            DeltaOptions recovery;
                            recovery.mode = DeltaMode::Upload;
                            recovery.resume = checkpoint;
                            recovery.cancelled = [&] { return control.checkpoint(); };
                            recovery.base.kind = checkpoint->base ? BaseSelector::Kind::Exact : BaseSelector::Kind::Absent;
                            if (checkpoint->base) recovery.base.versionId = *checkpoint->base;
                            Peer recoveryPeer(config, control);
                            const auto recovered = runDelta(source, [&](const Frame &f) { return recoveryPeer.request(f); }, recovery);
                            if (!recovered.result) throw std::runtime_error("missing recovered commit result");
                            committedSession = true;
                            if (!saved.value("completed").toBool()) {
                                auto receipt = r;
                                receipt.status = "已完成";
                                receipt.versionId = hex(recovered.result->versionId);
                                receipt.baseVersion = saved.value("base").toString();
                                receipt.time = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                                restorePlan(receipt, saved);
                                receipt.note = "已核验并恢复服务器提交记录";
                                saved["completed"] = true;
                                atomicJson(uploadPath, saved);
                                entry(receipt);
                            }
                            ownCompletedFull = config.mode == "full" && savedMode == "full";
                        } else if (status.phase == DeltaPhase::Collecting || status.phase == DeltaPhase::PlanReady ||
                                   status.phase == DeltaPhase::Transferring || status.phase == DeltaPhase::Verifying) {
                            pendingSession = true;
                            activeStatus = status;
                            r.resumeSession = hex(checkpoint->session);
                        } else throw std::runtime_error("session not resumable; retry after server recovery");
                    }
                    if (!sourceMatches) saved = {}; // Only obsolete completed receipts reach here.
                }
                const bool useResume = automatic || config.mode == "resume" || config.resume;
                if (!preview) {
                    if (automatic) {
                        const auto &p = pins.at(r.logicalPath);
                        const bool plannedResume = p.strategy == BackupStrategy::Resume;
                        if (p.strategy == BackupStrategy::Error || !p.modeAllowed ||
                            plannedResume != pendingSession ||
                            (plannedResume && p.resumeSession != r.resumeSession))
                            throw std::runtime_error("session or strategy changed after preview; scan again");
                        if ((p.strategy == BackupStrategy::Full && p.baseVersion != "absent") ||
                            ((p.strategy == BackupStrategy::Incremental || p.strategy == BackupStrategy::Skip) &&
                             p.baseVersion == "absent"))
                            throw std::runtime_error("invalid pinned strategy baseline");
                        identity["mode"] = p.strategy == BackupStrategy::Full ? "full" : "incremental";
                        if (plannedResume) {
                            // QUERY verifies the session; Preview also verifies the
                            // current remote baseline before sending resumed data.
                            DeltaOptions checkBase = options;
                            checkBase.mode = DeltaMode::Preview;
                            checkBase.save = [](const DeltaCheckpoint &) {};
                            Peer baselinePeer(config, control);
                            (void)runDelta(source, [&](const Frame &f) { return baselinePeer.request(f); }, checkBase);
                        }
                    }
                    if (config.mode == "resume" && !pendingSession && !committedSession)
                        throw std::runtime_error("no valid resume session");
                    if (pendingSession) {
                        if (!useResume || (config.mode == "full" && savedMode != "full"))
                            throw std::runtime_error("matching pending checkpoint requires resume mode");
                        if (!pins.at(r.logicalPath).resumeSession.isEmpty() &&
                            pins.at(r.logicalPath).resumeSession != r.resumeSession)
                            throw std::runtime_error("pending checkpoint changed after preview");
                        identity["mode"] = savedMode;
                        options.resume = checkpoint;
                    }
                }
                const bool noUpload = !preview && !pendingSession &&
                    pins.at(r.logicalPath).advice == BackupAdvice::Unchanged;
                if (noUpload) {
                    options.mode = DeltaMode::Preview;
                    options.resume.reset();
                }
                if (!preview && config.mode == "full" &&
                    pins.at(r.logicalPath).baseVersion != "absent" && (!ownCompletedFull || !noUpload))
                    throw std::runtime_error("full upload target exists; select incremental mode or a new destination");
                if (!preview && config.mode == "resume" && committedSession && !pendingSession && !noUpload)
                    throw std::runtime_error("no valid resume session; committed session requires a fresh preview");
                options.save = [&](const DeltaCheckpoint &cp) {
                    if (options.mode == DeltaMode::Preview)
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
                Peer peer(config, control);
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
                        if (options.mode == DeltaMode::Upload && reply.type == 20) {
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
                        if (options.mode == DeltaMode::Upload && reply.type == 21) {
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
                        if (options.mode == DeltaMode::Upload && f.type == 28 && reply.type == 29) {
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
                if (pendingSession && outcome.plan.baseVersion != checkpoint->base)
                    throw std::runtime_error("pending checkpoint baseline changed; use a new destination or recover original version");
                const bool unchanged = outcome.plan.baseVersion && outcome.plan.changeKind == "Unchanged" &&
                                       outcome.plan.missingBlocks == 0;
                r.advice = pendingSession ? BackupAdvice::Resume : !outcome.plan.baseVersion ? BackupAdvice::Full
                           : unchanged ? BackupAdvice::Unchanged : BackupAdvice::Incremental;
                r.modeAllowed = config.mode == "resume" ? (pendingSession || (committedSession && unchanged))
                    : config.mode == "full" ? (pendingSession ? useResume && savedMode == "full"
                        : !outcome.plan.baseVersion || (ownCompletedFull && unchanged))
                    : !pendingSession || useResume;
                r.strategy = !r.modeAllowed ? BackupStrategy::Error : pendingSession ? BackupStrategy::Resume
                    : !outcome.plan.baseVersion ? BackupStrategy::Full
                    : unchanged ? BackupStrategy::Skip : BackupStrategy::Incremental;
                if (!preview && automatic && r.strategy != pins.at(r.logicalPath).strategy)
                    throw std::runtime_error("strategy changed after preview; scan again");
                if (noUpload && !unchanged)
                    throw std::runtime_error("source or baseline changed after preview; scan again");
                r.missingBytes = outcome.plan.missingBytes;
                r.changedBlocks = outcome.plan.missingBlocks;
                if (preview && activeStatus &&
                    (activeStatus->phase == DeltaPhase::Transferring || activeStatus->phase == DeltaPhase::Verifying)) {
                    const auto bitmap = QByteArray::fromHex(QByteArray::fromStdString(activeStatus->missingBitmap));
                    const auto blocks = (r.size + config.chunkSize - 1) / config.chunkSize;
                    if (quint64(bitmap.size()) != (blocks + 7) / 8)
                        throw std::runtime_error("invalid resume bitmap geometry");
                    r.missingBytes = r.changedBlocks = 0;
                    for (quint64 i = 0; i < quint64(bitmap.size()) * 8; ++i)
                        if (static_cast<unsigned char>(bitmap[qsizetype(i/8)]) & (1U << (i%8))) {
                            if (i >= blocks) throw std::runtime_error("invalid resume bitmap padding");
                            ++r.changedBlocks;
                            r.missingBytes += std::min<quint64>(config.chunkSize, r.size - i*config.chunkSize);
                        }
                }
                r.change = outcome.plan.changeKind == "Added"       ? "新增"
                           : outcome.plan.changeKind == "Unchanged" ? "未变化"
                                                                    : "已修改";
                r.status = preview ? "就绪" : "已完成";
                r.note = preview
                             ? QString(pendingSession ? "续传尚需 %1 块，预计上传 %2 字节" : "变化 %1 块，预计上传 %2 字节")
                                   .arg(r.changedBlocks)
                                   .arg(r.missingBytes)
                             : QString("服务端已确认提交，本轮发送 %1 块").arg(outcome.sentBlocks);
                if (preview) {
                    progress.totalBytes += r.missingBytes;
                    if (!r.modeAllowed) {
                        r.status = "模式不匹配";
                        r.note += config.mode == "resume" ? "；没有可续传的未完成会话，请按建议重新选择模式并扫描。"
                            : pendingSession ? "；存在匹配未完成会话，请选择断点续传（或启用自动续传）。"
                            : "；首次上传保护已有目标，请选择增量备份或新的远端路径。";
                        entry(r);
                        ++result.failed;
                        continue;
                    }
                } else if (noUpload) {
                    r.status = "已跳过";
                    r.note = "已重新核验源摘要及服务器固定版本，无需上传。";
                    r.versionId.clear(); // No new COMMIT; do not manufacture a history receipt.
                    ++result.skipped;
                    ++progress.completedFiles;
                    entry(r);
                    log("信息", r.logicalPath + "：" + r.note);
                    continue;
                } else {
                    if (!r.modeAllowed || !outcome.result)
                        throw std::runtime_error("server did not commit or mode no longer allowed");
                    if (options.resume || saved.contains("plan")) {
                        const auto note = r.note;
                        restorePlan(r, saved);
                        if (r.change != "未知") r.note = note;
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
                r.modeAllowed = false;
                r.strategy = BackupStrategy::Error;
                r.note = friendly(e, config.direct);
                entry(r);
                log("错误", r.logicalPath + "：" + r.note);
                ++result.failed;
            }
        }
        result.planComplete = preview;
    } catch (const std::exception &e) {
        if (control.cancelled) {
            result.cancelled = true;
            log("警告", "任务已取消，已确认进度保留，可重试续传。");
        } else {
            ++result.failed;
            log("错误", friendly(e, config.direct));
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
