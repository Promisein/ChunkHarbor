#include "UploadHandler.h"
#include "chunkharbor/concurrency/DiskExecutor.h"
#include "chunkharbor/net/EventLoop.h"
#include "chunkharbor/protocol/Handshake.h"
#include "chunkharbor/storage/UploadStorage.h"
#include <algorithm>
#include <any>

namespace {
using namespace chunkharbor;
using namespace chunkharbor::protocol;
struct State {
    bool ready = false, upload = false, busy = false;
    std::uint64_t highestRequestId = 0;
    UploadSessionPtr session;
};
struct Reply {
    std::uint16_t type;
    std::string payload;
    UploadSessionPtr session;
};
void fail(const TcpConnection::Ptr &connection, std::uint64_t id, const std::string &code,
          const std::string &message) {
    if (connection->closed())
        return;
    connection->send(encode({15, id, encodeUploadError({code, message})}));
    connection->shutdownAfterWrite();
}
} // namespace

chunkharbor::TcpConnection::MessageCallback makeUploadHandler(chunkharbor::UploadStorage &storage,
                                                              chunkharbor::DiskExecutor &executor,
                                                              chunkharbor::EventLoop &loop) {
    auto state = std::make_shared<State>();
    return [state, &storage, &executor, &loop](const TcpConnection::Ptr &connection,
                                               Buffer &input) {
        while (!connection->closed() && !state->busy) {
            auto decoded = decode(input.readableView());
            if (decoded.status == DecodeStatus::NeedMore)
                return;
            if (decoded.status == DecodeStatus::Error) {
                connection->close();
                return;
            }
            input.consume(decoded.consumed);
            auto request = std::move(decoded.frame);
            try {
                if (!request.requestId || request.requestId <= state->highestRequestId)
                    throw MessageError("MALFORMED_MESSAGE",
                                       "request_id must increase and be nonzero");
                state->highestRequestId = request.requestId;
                if (!state->ready) {
                    if (request.type != 1)
                        throw MessageError("HANDSHAKE_REQUIRED", "Expected HELLO");
                    auto offer = decodeHello(request.payload);
                    auto ack = negotiateHello(offer);
                    state->upload = std::find(offer.capabilities.begin(), offer.capabilities.end(),
                                              "upload") != offer.capabilities.end();
                    if (state->upload)
                        ack.capabilities.push_back("upload");
                    connection->send(encode({2, request.requestId, encodeHelloAck(ack)}));
                    state->ready = true;
                    continue;
                }
                if (request.type == 1)
                    throw MessageError("ALREADY_HANDSHAKEN", "HELLO is only allowed once");
                if (!state->upload || (request.type != 3 && request.type != 5 && request.type != 9))
                    throw MessageError("UNSUPPORTED_OPERATION",
                                       "Operation not negotiated or unsupported");
                if (request.type == 3 && state->session)
                    throw MessageError("CONFLICT", "One file is allowed per connection");
                if (request.type != 3 && !state->session)
                    throw MessageError("SESSION_NOT_FOUND", "No upload session");
                const auto id = request.requestId;
                const std::weak_ptr<TcpConnection> weakConnection = connection;
                auto session = state->session;
                state->busy = true;
                connection->pauseReading();
                const auto submitted = executor.trySubmit(
                    [&storage, session, request = std::move(request)]() -> std::any {
                        try {
                            if (request.type == 3) {
                                auto started = storage.begin(decodeBeginUpload(request.payload));
                                return Reply{4, encodeUploadPlan(started.plan),
                                             std::move(started.session)};
                            }
                            if (request.type == 5)
                                return Reply{6,
                                             encodeChunkAck(storage.write(
                                                 session, decodeDataChunk(request.payload))),
                                             {}};
                            return Reply{10,
                                         encodeCommitResult(storage.commit(
                                             session, decodeCommitUpload(request.payload))),
                                         {}};
                        } catch (const BeginUploadError &) {
                            throw MessageError("MALFORMED_MESSAGE", "Invalid BEGIN_UPLOAD");
                        } catch (const DataChunkError &) {
                            throw MessageError("MALFORMED_MESSAGE", "Invalid DATA_CHUNK");
                        }
                    },
                    [state, weakConnection, id, &loop](DiskExecutor::Result result) {
                        auto peer = weakConnection.lock();
                        state->busy = false;
                        if (!peer || peer->closed())
                            return;
                        try {
                            if (result.error)
                                std::rethrow_exception(result.error);
                            auto reply = std::any_cast<Reply>(std::move(result.value));
                            if (reply.session)
                                state->session = std::move(reply.session);
                            peer->send(encode({reply.type, id, std::move(reply.payload)}));
                            // Release this completion's executor capacity before
                            // replaying a buffered follow-up request.
                            loop.queueInLoop([weakConnection] {
                                if (auto next = weakConnection.lock(); next && !next->closed())
                                    next->resumeReading();
                            });
                        } catch (const UploadFailure &error) {
                            fail(peer, id, error.code, "Upload operation rejected");
                        } catch (const MessageError &error) {
                            fail(peer, id, error.code, "Invalid upload request");
                        } catch (const std::exception &) {
                            fail(peer, id, "INTERNAL_ERROR", "Upload operation failed");
                        }
                    });
                if (submitted != DiskExecutor::Submit::Accepted) {
                    state->busy = false;
                    fail(connection, id, "BUSY", "Disk queue is full or stopping");
                }
                return;
            } catch (const MessageError &error) {
                fail(connection, request.requestId, error.code, error.what());
                return;
            } catch (const std::exception &) {
                fail(connection, request.requestId, "INTERNAL_ERROR", "Request processing failed");
                return;
            }
        }
    };
}
