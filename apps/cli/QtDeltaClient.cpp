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
#include <windows.h>
namespace {
using namespace chunkharbor::protocol;
std::string narrow(const QByteArray &bytes) {
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}
class Peer {
    QTcpSocket socket_;
    std::string input_;

  public:
    explicit Peer(quint16 port) {
        socket_.setReadBufferSize(static_cast<qint64>(HeaderSize + MaxControlPayload));
        socket_.connectToHost(QHostAddress::LocalHost, port);
        if (!socket_.waitForConnected(5000))
            throw std::runtime_error("connect failed");
        Hello offer;
        offer.capabilities = {"handshake", "upload", "fixed_block_delta_v1"};
        chunkharbor::client::HandshakeExchange exchange(offer, 1);
        auto hello = exchange.request();
        auto reply = request(hello.type, hello.requestId, hello.payload);
        exchange.accept(reply);
        if (std::find(exchange.ack().capabilities.begin(), exchange.ack().capabilities.end(),
                      "fixed_block_delta_v1") == exchange.ack().capabilities.end())
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
            if (socket_.bytesAvailable() == 0 && !socket_.waitForReadyRead(30000))
                throw std::runtime_error("server response timed out");
            input_ += narrow(
                socket_.read(static_cast<qint64>(HeaderSize + MaxControlPayload - input_.size())));
        }
    }
};
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
    Peer peer;
    std::vector<std::shared_ptr<NativeHandle>> parents;
    std::shared_ptr<NativeHandle> file;
    std::uint64_t size = 0;
    Owned(const std::filesystem::path &path, std::uint16_t port) : peer(port) {
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
} // namespace
DeltaPlatform openDeltaPlatform(const std::filesystem::path &path, const std::string &host,
                                std::uint16_t port, std::uint32_t chunk,
                                const std::string &logical) {
    if (host != "127.0.0.1")
        throw std::runtime_error("loopback endpoint required");
    auto owned = std::make_shared<Owned>(path, port);
    auto size = owned->size;
    if (!chunk || (size + chunk - 1) / chunk > 65536)
        throw std::runtime_error("invalid source geometry");
    auto digest = owned->digest();
    BeginUpload m{logical, static_cast<std::uint64_t>(size), 0, chunk, digest};
    (void)encodeBeginUpload(m);
    chunkharbor::client::SourceSnapshot source{
        m, [owned](auto offset, auto count) { return owned->read(offset, count); },
        [](std::string_view bytes) {
            auto hash = QCryptographicHash::hash(
                QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())),
                QCryptographicHash::Sha256);
            chunkharbor::client::Digest result{};
            std::copy(hash.begin(), hash.end(), result.begin());
            return result;
        },
        [owned, size, digest] {
            if (owned->size != size || owned->digest() != digest)
                throw std::runtime_error("source changed");
        }};
    return {std::move(source),
            [owned](const Frame &f) { return owned->peer.request(f.type, f.requestId, f.payload); },
            owned};
}
void atomicDeltaState(const std::filesystem::path &path, const std::string &value) {
    QSaveFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(value.data(), static_cast<qint64>(value.size())) !=
            static_cast<qint64>(value.size()) ||
        !file.commit())
        throw std::runtime_error("state publication failed");
}
