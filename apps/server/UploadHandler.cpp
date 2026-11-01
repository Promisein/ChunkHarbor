#include "UploadHandler.h"
#include "chunkharbor/concurrency/DiskExecutor.h"
#include "chunkharbor/net/EventLoop.h"
#include "chunkharbor/protocol/Handshake.h"
#include "chunkharbor/storage/DeltaStorage.h"
#include "chunkharbor/storage/UploadStorage.h"
#include <algorithm>
#include <any>

namespace {
using namespace chunkharbor;
using namespace chunkharbor::protocol;
struct State {
    bool ready = false, upload = false, busy = false, delta = false;
    std::uint64_t highestRequestId = 0;
    UploadSessionPtr session;
    std::optional<SessionId> deltaSession{};
};
struct Reply {
    std::uint16_t type;
    std::string payload;
    UploadSessionPtr session;
    std::optional<SessionId> deltaSession{};
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
                                                              chunkharbor::EventLoop &loop,
                                                              chunkharbor::DeltaStorage *delta) {
    auto state = std::make_shared<State>();
    return [state, &storage, &executor, &loop, delta](const TcpConnection::Ptr &connection,
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
                    state->delta =
                        delta && std::find(offer.capabilities.begin(), offer.capabilities.end(),
                                           "fixed_block_delta_v1") != offer.capabilities.end();
                    if (state->delta)
                        ack.capabilities.push_back("fixed_block_delta_v1");
                    connection->send(encode({2, request.requestId, encodeHelloAck(ack)}));
                    state->ready = true;
                    continue;
                }
                if (request.type == 1)
                    throw MessageError("ALREADY_HANDSHAKEN", "HELLO is only allowed once");
                const bool deltaRequest = request.type == 18 || request.type == 19 ||
                                          (request.type >= 24 && request.type <= 30) ||
                                          (request.type == 5 && state->deltaSession.has_value());
                if (deltaRequest && !state->delta)
                    throw MessageError("UNSUPPORTED_OPERATION", "Delta capability required");
                if (deltaRequest && state->session)
                    throw MessageError("CONFLICT", "Cannot mix upload sessions");
                if (!deltaRequest && state->deltaSession)
                    throw MessageError("CONFLICT", "Cannot mix upload sessions");
                if (deltaRequest && request.type == 24 && state->deltaSession)
                    throw MessageError("CONFLICT", "One file is allowed per connection");
                if (deltaRequest && request.type != 24 && request.type != 28 &&
                    request.type != 30 && !state->deltaSession)
                    throw MessageError("SESSION_NOT_FOUND", "Query delta session first");
                if (!deltaRequest && (!state->upload || (request.type != 3 && request.type != 5 &&
                                                         request.type != 7 && request.type != 9)))
                    throw MessageError("UNSUPPORTED_OPERATION",
                                       "Operation not negotiated or unsupported");
                if (!deltaRequest && (request.type == 3 || request.type == 7) && state->session)
                    throw MessageError("CONFLICT", "One file is allowed per connection");
                if (!deltaRequest && request.type != 3 && request.type != 7 && request.type != 9 &&
                    !state->session)
                    throw MessageError("SESSION_NOT_FOUND", "No upload session");
                const auto id = request.requestId;
                const std::weak_ptr<TcpConnection> weakConnection = connection;
                auto session = state->session;
                auto boundDelta = state->deltaSession;
                state->busy = true;
                connection->pauseReading();
                const auto submitted = executor.trySubmit(
                    [&storage, session, delta, deltaRequest, boundDelta,
                     request = std::move(request)]() -> std::any {
                        try {
                            if (deltaRequest) {
                                const auto bind = [&](const SessionId &id) {
                                    if (boundDelta && *boundDelta != id)
                                        throw UploadFailure("CONFLICT", "Delta session mismatch");
                                };
                                if (request.type == 24) {
                                    auto ready = delta->begin(decodeBeginDelta(request.payload));
                                    return Reply{25, encodeDeltaReady(ready), {}, ready.sessionId};
                                }
                                if (request.type == 28) {
                                    auto q = decodeQueryDelta(request.payload);
                                    bind(q.sessionId);
                                    auto status = delta->query({q.sessionId});
                                    return Reply{29, encodeDeltaStatus(status), {}, q.sessionId};
                                }
                                if (request.type == 18) {
                                    auto page = decodeManifestPage(request.payload);
                                    bind(page.sessionId);
                                    return Reply{26,
                                                 encodeManifestAck(delta->appendManifest(page)),
                                                 {},
                                                 page.sessionId};
                                }
                                if (request.type == 19) {
                                    auto done = decodeManifestDone(request.payload);
                                    bind(done.sessionId);
                                    return Reply{26,
                                                 encodeManifestAck(delta->finishManifest(done)),
                                                 {},
                                                 done.sessionId};
                                }
                                if (request.type == 27) {
                                    auto get = decodeGetPlanPage(request.payload);
                                    bind(get.sessionId);
                                    auto page = delta->planPage(get);
                                    if (auto *p = std::get_if<PlanPage>(&page))
                                        return Reply{20, encodePlanPage(*p), {}, get.sessionId};
                                    return Reply{21,
                                                 encodePlanDone(std::get<PlanDone>(page)),
                                                 {},
                                                 get.sessionId};
                                }
                                if (request.type == 5) {
                                    auto chunk = decodeDataChunk(request.payload);
                                    bind(chunk.sessionId);
                                    return Reply{6,
                                                 encodeChunkAck(delta->write(chunk)),
                                                 {},
                                                 chunk.sessionId};
                                }
                                if (request.type == 30) {
                                    auto commit = decodeCommitDelta(request.payload);
                                    bind(commit.sessionId);
                                    return Reply{
                                        31,
                                        encodeDeltaResult(delta->commit({commit.sessionId})),
                                        {},
                                        commit.sessionId};
                                }
                                throw MessageError("UNSUPPORTED_OPERATION",
                                                   "Unsupported delta request");
                            }
                            if (request.type == 3) {
                                auto started = storage.begin(decodeBeginUpload(request.payload));
                                return Reply{4, encodeUploadPlan(started.plan),
                                             std::move(started.session)};
                            }
                            if (request.type == 7) {
                                auto found = storage.query(decodeQuerySession(request.payload));
                                if (found.committed)
                                    return Reply{10, encodeCommitResult(found.result), {}};
                                return Reply{8, encodeSessionStatus(found.status),
                                             std::move(found.session)};
                            }
                            if (request.type == 9 && !session) {
                                auto found = storage.query(decodeCommitUpload(request.payload));
                                if (!found.committed)
                                    throw UploadFailure("SESSION_NOT_FOUND",
                                                        "query active session before commit");
                                return Reply{10, encodeCommitResult(found.result), {}};
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
                        } catch (const ManifestPageError &) {
                            throw MessageError("MALFORMED_MESSAGE", "Invalid MANIFEST_PAGE");
                        } catch (const UploadPlanError &) {
                            throw MessageError("MALFORMED_MESSAGE", "Invalid upload plan");
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
                            if (reply.deltaSession)
                                state->deltaSession = reply.deltaSession;
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
